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

std::unique_ptr<Http> MusicPlayer::OpenFollowingRedirects(
    const std::string& url, int timeout_ms, const std::string& accept_header) {
    // Use ONE HttpClient for all redirect hops. Creating/destroying a client
    // per hop (SSL transport + event group teardown) while the previous
    // receive task winds down caused a BREAK-instr assert on https->http hops.
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "No HTTP client");
        return nullptr;
    }
    http->SetTimeout(timeout_ms);

    std::string current = url;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        // Open() clears per-request headers after sending, so set them each hop.
        http->SetHeader("User-Agent", "Mozilla/5.0");
        http->SetHeader("Referer", "https://music.163.com");
        if (!accept_header.empty()) {
            http->SetHeader("Accept", accept_header);
        }

        auto opened = http->Open("GET", current);
        if (!opened) {
            ESP_LOGE(TAG, "HTTP open failed (hop %d): %s", hop,
                     opened.error().ToString().c_str());
            http->Close();
            return nullptr;
        }
        auto status = http->GetStatusCode();
        int code = status ? *status : -1;

        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            std::string location = http->GetResponseHeader("Location");
            ESP_LOGI(TAG, "Redirect %d (hop %d) -> %s", code, hop,
                     location.empty() ? "(none)" : location.c_str());
            http->Close();
            if (location.empty() || location == current) {
                ESP_LOGE(TAG, "Empty/looping redirect location");
                return nullptr;
            }
            // Resolve a relative Location against the current URL origin.
            if (location.rfind("http://", 0) != 0 && location.rfind("https://", 0) != 0) {
                auto scheme_end = current.find("://");
                auto slash = current.find('/', scheme_end == std::string::npos
                                                    ? 0
                                                    : scheme_end + 3);
                std::string origin = slash == std::string::npos
                                         ? current
                                         : current.substr(0, slash);
                location = (location[0] == '/') ? (origin + location)
                                                : (origin + "/" + location);
                ESP_LOGI(TAG, "Resolved relative redirect -> %s", location.c_str());
            }
            current = location;
            continue;
        }

        if (code < 200 || code >= 300) {
            ESP_LOGE(TAG, "HTTP status %d (hop %d)", code, hop);
            http->Close();
            return nullptr;
        }
        // 2xx: hand the ready-to-read client back to the caller.
        return http;
    }

    ESP_LOGE(TAG, "Too many redirects (>%d)", kMaxRedirects);
    http->Close();
    return nullptr;
}

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
    // Large stack: MP3 decode + resample use sizeable scratch buffers. 8 KB
    // overflowed the stack and caused a Guru Meditation (BREAK instr) crash.
    BaseType_t ok = xTaskCreate(TaskEntry, "music_play", 32768, this, 2, &task_handle_);
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
    // Prefer a free (fee==0) track, but fall back to any track whose outer
    // link may still be playable — many "VIP" tracks stream fine via the
    // outer URL, and being too strict made popular songs (e.g. 晴天) fail.
    std::string fallback_url, fallback_title;
    cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON* songs = result ? cJSON_GetObjectItemCaseSensitive(result, "songs") : nullptr;
    if (cJSON_IsArray(songs)) {
        int count = cJSON_GetArraySize(songs);
        for (int i = 0; i < count; ++i) {
            cJSON* song = cJSON_GetArrayItem(songs, i);
            if (!song) continue;
            cJSON* fee = cJSON_GetObjectItemCaseSensitive(song, "fee");
            cJSON* id = cJSON_GetObjectItemCaseSensitive(song, "id");
            cJSON* name = cJSON_GetObjectItemCaseSensitive(song, "name");
            if (!cJSON_IsNumber(id)) continue;

            // song id is 64-bit (e.g. 3455095614 > INT32_MAX): use valuedouble.
            long long song_id = static_cast<long long>(id->valuedouble);
            char idbuf[32];
            snprintf(idbuf, sizeof(idbuf), "%lld", song_id);
            // Keep the https origin: the CDN only serves the real MP3 when the
            // redirect came from https (plain http origin gets a 403). We
            // follow the https->http 302 manually in OpenFollowingRedirects.
            std::string url = std::string("https://music.163.com/song/media/outer/url?id=") +
                              idbuf + ".mp3";
            std::string title_str =
                cJSON_IsString(name) && name->valuestring ? name->valuestring : "未知歌曲";

            bool is_free = cJSON_IsNumber(fee) && fee->valueint == 0;
            if (is_free && !found) {
                mp3_url = url;
                title = title_str;
                found = true;
                break;
            }
            if (fallback_url.empty()) {
                fallback_url = url;
                fallback_title = title_str;
            }
        }
    }
    cJSON_Delete(root);
    if (!found && !fallback_url.empty()) {
        mp3_url = fallback_url;
        title = fallback_title;
        found = true;
        ESP_LOGI(TAG, "Using fallback (non-free) track: %s", title.c_str());
    }
    if (!found) {
        ESP_LOGW(TAG, "No song found for: %s", query.c_str());
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

    // Open the MP3 stream, following the NetEase 302 redirect to the CDN.
    auto http = OpenFollowingRedirects(mp3_url, 10000, "audio/mpeg,*/*");
    if (!http) {
        ESP_LOGE(TAG, "MP3 open/redirect failed");
        return false;
    }

    int out_rate = codec->output_sample_rate();
    if (out_rate <= 0) out_rate = 24000;

    void* dec = nullptr;
    esp_audio_err_t derr = esp_mp3_dec_open(nullptr, 0, &dec);
    if (derr != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "mp3 decoder open failed: %d", derr);
        return false;
    }

    // Enable speaker output.
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    // Allocate scratch on the HEAP, not the stack. Stack-allocated buffers
    // (4 KB + 8 KB) blew the task stack even after enlarging it, and are the
    // classic cause of the "BREAK instr" Guru Meditation during playback.
    constexpr size_t kInBufSize = 4096;
    std::vector<char> in_buf_vec(kInBufSize);
    std::vector<uint8_t> pcm_buf_vec(8192);
    char* in_buf = in_buf_vec.data();
    uint8_t* pcm_buf = pcm_buf_vec.data();
    int detected_rate = 0;
    int detected_channels = 0;
    bool played_any = false;
    bool ok = true;

    // Some CDN tracks (VIP trial snippets, m4a mislabeled as mp3) fail to
    // decode. Without forward progress the decoder is re-fed the same bad
    // bytes forever, spamming "Not supported format" and hanging the task.
    // Track consecutive failures and give up cleanly after a threshold.
    int consecutive_errors = 0;
    constexpr int kMaxConsecutiveErrors = 32;
    int total_errors = 0;

    while (!cancelled_.load()) {
        auto got = http->Read(in_buf, kInBufSize);
        if (!got) {
            ESP_LOGE(TAG, "MP3 read failed: %s", got.error().ToString().c_str());
            ok = false;
            break;
        }
        if (*got == 0) break;

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = reinterpret_cast<uint8_t*>(in_buf);
        raw.len = static_cast<uint32_t>(*got);

        // Feed all input, emitting decoded PCM frames.
        int guard = 0;
        while (raw.len > 0 && guard++ < 64 && !cancelled_.load()) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = pcm_buf;
            frame.len = static_cast<uint32_t>(pcm_buf_vec.size());
            esp_audio_dec_info_t info = {};
            esp_audio_err_t r = esp_mp3_dec_decode(dec, &raw, &frame, &info);
            if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH || r == ESP_AUDIO_ERR_DATA_LACK ||
                r == ESP_AUDIO_ERR_CONTINUE) {
                // Need more data to complete a frame; wait for the next chunk.
                break;
            }
            if (r != ESP_AUDIO_ERR_OK) {
                // Bad frame. ALWAYS advance at least one byte so we never get
                // stuck re-decoding the same bytes.
                if (raw.consumed > 0 && raw.consumed <= raw.len) {
                    raw.buffer += raw.consumed;
                    raw.len -= raw.consumed;
                } else {
                    raw.buffer += 1;
                    raw.len -= 1;
                }
                if (++total_errors <= 3) {
                    ESP_LOGW(TAG, "MP3 decode error %d (skipping)", (int)r);
                }
                if (++consecutive_errors >= kMaxConsecutiveErrors) {
                    ESP_LOGE(TAG, "Too many decode errors (%d), aborting this track",
                             consecutive_errors);
                    ok = false;
                    break;
                }
                continue;
            }
            // A successful decode resets the consecutive-failure counter.
            consecutive_errors = 0;
            if (frame.decoded_size == 0 || raw.consumed == 0) {
                raw.buffer += (raw.consumed > 0 ? raw.consumed : 1);
                raw.len -= (raw.consumed > 0 ? raw.consumed : 1);
                break;
            }

            if (detected_rate == 0 && info.sample_rate > 0) {
                detected_rate = static_cast<int>(info.sample_rate);
                detected_channels = info.channel > 0 ? info.channel : 1;
                ESP_LOGI(TAG, "MP3 %d Hz, %d ch -> out %d Hz", detected_rate,
                         detected_channels, out_rate);
            }
            int rate = detected_rate > 0 ? detected_rate : 44100;
            int ch = detected_channels > 0 ? detected_channels : 2;

            // AudioService's power timer may disable output while we stream
            // (we bypass its output task). Re-enable PA if it was turned off.
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
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
        if (consecutive_errors >= kMaxConsecutiveErrors) break;
    }

    esp_mp3_dec_close(dec);
    http->Close();
    ESP_LOGI(TAG, "Playback finished, played_any=%d ok=%d errors=%d",
             played_any ? 1 : 0, ok ? 1 : 0, total_errors);
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
    auto& board = Board::GetInstance();

    // Streaming MP3 needs a stable WiFi link. LOW_POWER (MAX_MODEM) can drop
    // the connection mid-stream -> "Server disconnected" -> audio cuts out.
    // Lock WiFi to full performance while music plays, restore afterwards.
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    // Stop the assistant's TTS so the speaker is free for music. AbortSpeaking
    // is safe from this task. Give the in-flight TTS a moment to drain before
    // we start writing PCM to the I2S output directly.
    app.AbortSpeaking(kAbortReasonWakeWordDetected);
    vTaskDelay(pdMS_TO_TICKS(1500));

    std::string mp3_url, title;
    bool success = false;
    if (SearchSong(query, mp3_url, title)) {
        ESP_LOGI(TAG, "Playing: %s -> %s", title.c_str(), mp3_url.c_str());
        success = DownloadAndPlay(mp3_url);
    } else {
        ESP_LOGW(TAG, "No playable song found for: %s", query.c_str());
    }
    // Restore low-power WiFi after playback.
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(TAG, "Music playback %s: %s", success ? "ok" : "failed", title.c_str());
}
