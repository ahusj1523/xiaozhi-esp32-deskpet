#include "mp3_music_player.h"

#include <cstring>
#include <memory>
#include <vector>

#include <esp_err.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "esp_ae_rate_cvt.h"
#include "minimp3/minimp3.h"

#define TAG "Mp3Music"
#define MP3_TASK_STACK 32768
#define MP3_BUF_SIZE (32 * 1024)

Mp3MusicPlayer& Mp3MusicPlayer::GetInstance() {
    static Mp3MusicPlayer instance;
    return instance;
}

bool Mp3MusicPlayer::PlayUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        ESP_LOGE(TAG, "URL 不是 http/https: %s", url.c_str());
        return false;
    }
    Stop();
    for (int i = 0; i < 100 && task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (task_handle_ != nullptr) {
        ESP_LOGE(TAG, "旧播放任务未退出，放弃本次播放");
        return false;
    }

    stop_req_.store(false);
    playing_.store(true);
    position_ms_.store(0);
    duration_ms_.store(0);

    auto* arg = new std::string(url);
    if (xTaskCreate(&Mp3MusicPlayer::TaskEntry, "mp3_music", MP3_TASK_STACK, arg, 5,
                    &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "创建 mp3 播放任务失败");
        delete arg;
        playing_.store(false);
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void Mp3MusicPlayer::Stop() {
    if (playing_.load()) {
        stop_req_.store(true);
    }
}

void Mp3MusicPlayer::TaskEntry(void* arg) {
    std::unique_ptr<std::string> url(static_cast<std::string*>(arg));
    GetInstance().Run(*url);
    GetInstance().task_handle_ = nullptr;
    GetInstance().playing_.store(false);
    GetInstance().stop_req_.store(false);
    vTaskDelete(nullptr);
}

void Mp3MusicPlayer::Run(std::string url) {
    ESP_LOGI(TAG, "开始拉流 mp3: %s", url.c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.keep_alive_enable = false;
    config.user_agent = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36";
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "http client 初始化失败");
        return;
    }
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开音频流失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "HTTP 状态异常: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    auto* codec = Board::GetInstance().GetAudioCodec();
    int out_rate = codec->output_sample_rate();
    int out_channels = codec->output_channels();

    // 等云端 TTS 播完再出声，避免两路同时写 I2S
    for (int i = 0; i < 120 && !audio.IsPlaybackIdle() && !stop_req_.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio.ResetDecoder();
    codec->EnableOutput(true);

    mp3dec_t dec;
    mp3dec_init(&dec);

    auto buf = std::make_unique<uint8_t[]>(MP3_BUF_SIZE);
    size_t remaining = 0;
    size_t pos = 0;
    bool eof = false;
    bool first_sound = false;

    std::vector<int16_t> pcm;
    pcm.resize(MINIMP3_MAX_SAMPLES_PER_FRAME);
    std::vector<int16_t> mono;
    std::vector<int16_t> out;
    void* resampler = nullptr;
    int resampler_src_rate = 0;

    auto close_resampler = [&]() {
        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
            resampler_src_rate = 0;
        }
    };

    while (!stop_req_.load()) {
        // 数据不够一帧时先整理缓冲再补数据
        if (remaining - pos < 4096) {
            if (pos > 0) {
                std::memmove(buf.get(), buf.get() + pos, remaining - pos);
                remaining -= pos;
                pos = 0;
            }
            if (!eof && remaining < MP3_BUF_SIZE) {
                int r = esp_http_client_read(client, reinterpret_cast<char*>(buf.get() + remaining),
                                             static_cast<int>(MP3_BUF_SIZE - remaining));
                if (r <= 0) {
                    eof = true;
                } else {
                    remaining += r;
                }
            }
        }

        mp3dec_frame_info_t info = {};
        int samples = mp3dec_decode_frame(&dec, buf.get() + pos,
                                          static_cast<int>(remaining - pos), pcm.data(), &info);
        if (samples > 0) {
            pos += info.frame_bytes;
            if (!first_sound) {
                ESP_LOGI(TAG, "解码成功：%d Hz / %d ch / %d kbps，输出 %d Hz %d ch",
                         info.hz, info.channels, info.bitrate_kbps, out_rate, out_channels);
                first_sound = true;
            }

            // 统一成单声道
            mono.resize(samples);
            if (info.channels == 1) {
                std::memcpy(mono.data(), pcm.data(), samples * sizeof(int16_t));
            } else {
                for (int i = 0; i < samples; ++i) {
                    int32_t sum = static_cast<int32_t>(pcm[2 * i]) + pcm[2 * i + 1];
                    mono[i] = static_cast<int16_t>(sum / 2);
                }
            }

            // 重采样到 codec 输出采样率
            if (info.hz > 0 && info.hz != out_rate) {
                if (resampler == nullptr || resampler_src_rate != info.hz) {
                    close_resampler();
                    esp_ae_rate_cvt_cfg_t cfg = {
                        .src_rate = static_cast<uint32_t>(info.hz),
                        .dest_rate = static_cast<uint32_t>(out_rate),
                        .channel = ESP_AUDIO_MONO,
                        .bits_per_sample = ESP_AUDIO_BIT16,
                        .complexity = 1,
                        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                    };
                    int rc = static_cast<int>(esp_ae_rate_cvt_open(&cfg, &resampler));
                    if (rc != 0 || resampler == nullptr) {
                        ESP_LOGE(TAG, "创建重采样器失败 %d->%d", info.hz, out_rate);
                        resampler = nullptr;
                        break;
                    }
                    resampler_src_rate = info.hz;
                }
                uint32_t max_out = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(resampler, static_cast<uint32_t>(samples),
                                                       &max_out);
                out.resize(max_out > 0 ? max_out : static_cast<size_t>(samples) * 2);
                uint32_t actual = 0;
                esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(mono.data()),
                                        static_cast<uint32_t>(samples),
                                        reinterpret_cast<esp_ae_sample_t>(out.data()), &actual);
                out.resize(actual);
            } else {
                out = mono;
            }

            if (!out.empty()) {
                // 重采样后需要与输出声道对齐
                if (out_channels > 1) {
                    std::vector<int16_t> interleaved(out.size() * out_channels);
                    for (size_t i = 0; i < out.size(); ++i) {
                        for (int c = 0; c < out_channels; ++c) {
                            interleaved[i * out_channels + c] = out[i];
                        }
                    }
                    codec->OutputData(interleaved);
                } else {
                    codec->OutputData(out);
                }
            }

            if (info.hz > 0) {
                position_ms_.store(position_ms_.load() + samples * 1000 / info.hz);
            }
            continue;
        }

        if (info.frame_bytes > 0) {
            // 坏帧或非音频数据，跳过
            pos += info.frame_bytes;
            continue;
        }

        // 需要更多数据
        if (eof && remaining - pos <= 1) {
            break;
        }
        if (eof) {
            // 文件尾还有残留字节，逐字节丢，防止死循环
            pos += 1;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    close_resampler();
    if (stop_req_.load()) {
        ESP_LOGI(TAG, "mp3 播放被停止（已播 %d ms）", position_ms_.load());
    } else {
        ESP_LOGI(TAG, "mp3 播放结束（共 %d ms）", position_ms_.load());
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}
