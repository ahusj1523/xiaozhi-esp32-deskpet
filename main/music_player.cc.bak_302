#include "music_player.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_audio_dec.h>
#include <decoder/impl/esp_mp3_dec.h>

#include "application.h"
#include "board.h"
#include "audio_codec.h"
#include "http.h"

#define TAG "MusicPlayer"

namespace {

// URL-encode a UTF-8 string (percent-encode all non-unreserved bytes).
std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Simple linear-interpolation resampler + stereo->mono.
// Input: interleaved 16-bit PCM, in_channels (1 or 2), in_rate.
// Output: mono 16-bit PCM at out_rate, appended to out.
void ResampleToMono(const int16_t* src, size_t src_frames, int in_channels,
                    int in_rate, int out_rate, std::vector<int16_t>& out) {
    if (in_rate <= 0 || out_rate <= 0 || src_frames == 0) return;
    double ratio = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    size_t out_frames = static_cast<size_t>(src_frames / ratio);
    if (out_frames == 0) return;
    out.reserve(out.size() + out_frames);

    auto sample_at = [&](double pos) -> int16_t {
        int64_t i0 = static_cast<int64_t>(pos);
        if (i0 < 0) i0 = 0;
        if (i0 >= static_cast<int64_t>(src_frames) - 1) i0 = src_frames - 1;
        int64_t i1 = i0 + 1;
        double frac = pos - i0;
        // Mix down to mono first.
        auto mono = [&](int64_t idx) -> double {
            double v = 0;
            for (int c = 0; c < in_channels; ++c) {
                v += src[idx * in_channels + c];
            }
            return v / in_channels;
        };
        double v = mono(i0) * (1.0 - frac) + mono(i1) * frac;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        return static_cast<int16_t>(v);
    };

    for (size_t i = 0; i < out_frames; ++i) {
        out.push_back(sample_at(static_cast<double>(i) * ratio));
    }
}

}  // namespace

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

bool MusicPlayer::Play(const std::string& query) {
    if (busy_.load()) {
        ESP_LOGW(TAG, "Already playing, ignore: %s", query.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        query_ = query;
    }
    cancelled_.store(false);
    busy_.store(true);
    BaseType_t ok = xTaskCreate(TaskEntry, "music_play", 8192, this, 2, &task_handle_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create music task");
        busy_.store(false);
        return false;
    }
    return true;
}

void MusicPlayer::Stop() {
    cancelled_.store(true);
}

void MusicPlayer::TaskEntry(void* arg) {
    auto* self = static_cast<MusicPlayer*>(arg);
    self->Task();
    self->task_handle_ = nullptr;
    self->busy_.store(false);
    vTaskDelete(nullptr);
}

bool MusicPlayer::SearchSong(const std::string& query, std::string& mp3_url,
                             std::string& title) {
    auto& board = Board::GetInstance();
    auto http = board.GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "No HTTP client");
        return false;
    }
    http->SetTimeout(8000);
    http->SetHeader("User-Agent", "Mozilla/5.0");
    http->SetHeader("Referer", "https://music.163.com");

    std::string url = "https://music.163.com/api/search/get/web?s=" + UrlEncode(query) +
                      "&type=1&offset=0&limit=10";
    auto opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGE(TAG, "Search open failed: %s", opened.error().ToString().c_str());
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGE(TAG, "Search HTTP status: %d", status ? *status : -1);
        http->Close();
        return false;
    }
    std::string text = http->ReadAll();
    http->Close();
    if (text.empty()) {
        ESP_LOGE(TAG, "Search read failed: empty body");
        return false;
    }
    ESP_LOGI(TAG, "Search response %u bytes", (unsigned)text.size());

    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Search JSON parse failed");
        return false;
    }
    bool found = false;
    cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON* songs = result ? cJSON_GetObjectItemCaseSensitive(result, "songs") : nullptr;
    if (cJSON_IsArray(songs)) {
        int count = cJSON_GetArraySize(songs);
        for (int i = 0; i < count && !found; ++i) {
            cJSON* song = cJSON_GetArrayItem(songs, i);
            if (!song) continue;
            cJSON* fee = cJSON_GetObjectItemCaseSensitive(song, "fee");
            cJSON* id = cJSON_GetObjectItemCaseSensitive(song, "id");
            cJSON* name = cJSON_GetObjectItemCaseSensitive(song, "name");
            // fee==0 means freely playable. Skip VIP/paid tracks.
            if (!cJSON_IsNumber(id) || !cJSON_IsNumber(fee) || fee->valueint != 0) continue;
            char idbuf[32];
            snprintf(idbuf, sizeof(idbuf), "%d", id->valueint);
            mp3_url = std::string("https://music.163.com/song/media/outer/url?id=") +
                      idbuf + ".mp3";
            title = cJSON_IsString(name) && name->valuestring ? name->valuestring : "未知歌曲";
            found = true;
        }
    }
    cJSON_Delete(root);
    if (!found) {
        ESP_LOGW(TAG, "No free (fee=0) song found for: %s", query.c_str());
    }
    return found;
}

bool MusicPlayer::DownloadAndPlay(const std::string& mp3_url) {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "No audio codec");
        return false;
    }

    // Open the MP3 stream.
    auto http = board.GetNetwork()->CreateHttp(0);
    if (!http) return false;
    http->SetTimeout(10000);
    http->SetHeader("User-Agent", "Mozilla/5.0");
    http->SetHeader("Accept", "audio/mpeg,*/*");
    auto opened = http->Open("GET", mp3_url);
    if (!opened) {
        ESP_LOGE(TAG, "MP3 open failed: %s", opened.error().ToString().c_str());
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGE(TAG, "MP3 HTTP status: %d", status ? *status : -1);
        http->Close();
        return false;
    }

    int out_rate = codec->output_sample_rate();
    if (out_rate <= 0) out_rate = 24000;

    void* dec = nullptr;
    esp_audio_err_t derr = esp_mp3_dec_open(nullptr, 0, &dec);
    if (derr != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "mp3 decoder open failed: %d", derr);
        http->Close();
        return false;
    }

    // Enable speaker output.
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    constexpr size_t kInBufSize = 4096;
    std::array<char, kInBufSize> in_buf;
    std::array<uint8_t, 8192> pcm_buf;  // decoded PCM scratch
    int detected_rate = 0;
    int detected_channels = 0;
    bool played_any = false;
    bool ok = true;

    while (!cancelled_.load()) {
        auto got = http->Read(in_buf.data(), in_buf.size());
        if (!got) {
            ESP_LOGE(TAG, "MP3 read failed: %s", got.error().ToString().c_str());
            ok = false;
            break;
        }
        if (*got == 0) break;

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = reinterpret_cast<uint8_t*>(in_buf.data());
        raw.len = static_cast<uint32_t>(*got);

        // Feed all input, emitting decoded PCM frames.
        int guard = 0;
        while (raw.len > 0 && guard++ < 64 && !cancelled_.load()) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = pcm_buf.data();
            frame.len = static_cast<uint32_t>(pcm_buf.size());
            esp_audio_dec_info_t info = {};
            esp_audio_err_t r = esp_mp3_dec_decode(dec, &raw, &frame, &info);
            if (r != ESP_AUDIO_ERR_OK) {
                if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) break;
                // Not enough data for a full frame yet: stop feeding this chunk.
                break;
            }
            if (frame.decoded_size == 0 || raw.consumed == 0) break;

            if (detected_rate == 0 && info.sample_rate > 0) {
                detected_rate = static_cast<int>(info.sample_rate);
                detected_channels = info.channel > 0 ? info.channel : 1;
                ESP_LOGI(TAG, "MP3 %d Hz, %d ch -> out %d Hz", detected_rate,
                         detected_channels, out_rate);
            }
            int rate = detected_rate > 0 ? detected_rate : 44100;
            int ch = detected_channels > 0 ? detected_channels : 2;

            std::vector<int16_t> pcm_out;
            const int16_t* samples = reinterpret_cast<const int16_t*>(frame.buffer);
            size_t frames = frame.decoded_size / (2 * static_cast<size_t>(ch));
            ResampleToMono(samples, frames, ch, rate, out_rate, pcm_out);
            if (!pcm_out.empty()) {
                codec->OutputData(pcm_out);
                played_any = true;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
    }

    esp_mp3_dec_close(dec);
    http->Close();
    ESP_LOGI(TAG, "Playback finished, played_any=%d ok=%d", played_any ? 1 : 0, ok ? 1 : 0);
    return played_any && ok && !cancelled_.load();
}

void MusicPlayer::Task() {
    std::string query;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        query = query_;
    }
    ESP_LOGI(TAG, "Play request: %s", query.c_str());

    auto& app = Application::GetInstance();

    // Enter notifying state so the assistant stops talking and the speaker is
    // reserved for music. This mirrors the notification playback flow.
    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.AbortSpeaking(kAbortReasonWakeWordDetected);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!app.SetDeviceState(kDeviceStateNotifying)) {
        ESP_LOGW(TAG, "Cannot enter notifying state, state=%d", app.GetDeviceState());
    }

    std::string mp3_url, title;
    bool success = false;
    if (SearchSong(query, mp3_url, title)) {
        ESP_LOGI(TAG, "Playing: %s -> %s", title.c_str(), mp3_url.c_str());
        success = DownloadAndPlay(mp3_url);
    }

    // Restore to idle so voice interaction resumes.
    app.Schedule([success, title]() {
        auto& a = Application::GetInstance();
        if (a.GetDeviceState() == kDeviceStateNotifying) {
            a.SetDeviceState(kDeviceStateIdle);
        }
        ESP_LOGI(TAG, "Music playback %s: %s", success ? "ok" : "failed", title.c_str());
    });
}
