#include "http_music_player.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio_service.h"
#include "demuxer/ogg_demuxer.h"

#define TAG "HttpMusic"
#define READ_CHUNK_SIZE 4096
#define MUSIC_TASK_STACK 16384

HttpMusicPlayer& HttpMusicPlayer::GetInstance() {
    static HttpMusicPlayer instance;
    return instance;
}

bool HttpMusicPlayer::PlayUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        ESP_LOGE(TAG, "URL 不是 http/https: %s", url.c_str());
        return false;
    }

    Stop();
    // 等旧任务收尾，避免两个读流任务同时往解码队列塞包
    for (int i = 0; i < 100 && task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (task_handle_ != nullptr) {
        ESP_LOGE(TAG, "旧播放任务未退出，放弃本次播放");
        return false;
    }

    stop_req_.store(false);
    playing_.store(true);

    auto* arg = new std::string(url);
    if (xTaskCreate(&HttpMusicPlayer::TaskEntry, "http_music", MUSIC_TASK_STACK, arg, 5,
                    &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "创建播放任务失败");
        delete arg;
        playing_.store(false);
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void HttpMusicPlayer::Stop() {
    if (playing_.load()) {
        stop_req_.store(true);
    }
}

void HttpMusicPlayer::TaskEntry(void* arg) {
    std::unique_ptr<std::string> url(static_cast<std::string*>(arg));
    GetInstance().Run(*url);
    GetInstance().task_handle_ = nullptr;
    GetInstance().playing_.store(false);
    GetInstance().stop_req_.store(false);
    vTaskDelete(nullptr);
}

void HttpMusicPlayer::Run(std::string url) {
    ESP_LOGI(TAG, "开始拉流: %s", url.c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.buffer_size = READ_CHUNK_SIZE;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "http client 初始化失败");
        return;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    // 清掉可能残留的 TTS/上一次音乐
    audio.ResetDecoder();

    bool finished = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        int content_length = esp_http_client_fetch_headers(client);
        ESP_LOGI(TAG, "HTTP 已连接, content_length=%d, status=%d", content_length,
                 esp_http_client_get_status_code(client));

        // OggDemuxer 内部有 ~2.5KB 缓冲，放堆上，避免栈帧超限
        auto demuxer_holder = std::make_unique<OggDemuxer>();
        OggDemuxer& demuxer = *demuxer_holder;
        demuxer.OnPacket([&audio](const uint8_t* data, int sample_rate, int frame_duration_ms,
                                  size_t len) {
            if (len == 0) {
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration_ms;
            packet->payload.assign(data, data + len);
            audio.PushPacketToDecodeQueue(std::move(packet), true);
        });
        demuxer.Reset();

        char* buf = static_cast<char*>(malloc(READ_CHUNK_SIZE));
        if (buf != nullptr) {
            while (!stop_req_.load()) {
                int n = esp_http_client_read(client, buf, READ_CHUNK_SIZE);
                if (n < 0) {
                    ESP_LOGE(TAG, "读流失败");
                    break;
                }
                if (n == 0) {
                    finished = true;
                    break;
                }
                demuxer.Process(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
            }
            free(buf);
        }
        esp_http_client_close(client);
    } else {
        ESP_LOGE(TAG, "HTTP 连接失败: %s", url.c_str());
    }

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "播放结束, finished=%d, stop_req=%d", finished ? 1 : 0,
             stop_req_.load() ? 1 : 0);
}
