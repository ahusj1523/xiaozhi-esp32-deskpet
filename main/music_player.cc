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

bool MusicPlayer::SearchSongs(const std::string& query,
                              std::vector<SongCandidate>& out) {
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
                      "&type=1&offset=0&limit=20";
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

    // Collect free tracks first, then paid ones as fallback. A single bad
    // link (VIP trial / m4a) used to make playback fail entirely; trying
    // several candidates means we can skip broken streams and still play.
    constexpr int kMaxCandidates = 6;
    std::vector<SongCandidate> free_list, paid_list;
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
            std::string song_url =
                std::string("https://music.163.com/song/media/outer/url?id=") +
                idbuf + ".mp3";
            std::string title_str =
                cJSON_IsString(name) && name->valuestring ? name->valuestring : "未知歌曲";

            bool is_free = cJSON_IsNumber(fee) && fee->valueint == 0;
            SongCandidate c{song_url, title_str};
            if (is_free) {
                if (free_list.size() < kMaxCandidates) free_list.push_back(c);
            } else {
                if (paid_list.size() < kMaxCandidates) paid_list.push_back(c);
            }
        }
    }
    cJSON_Delete(root);

    out = free_list;
    for (auto& c : paid_list) {
        if (out.size() >= kMaxCandidates) break;
        out.push_back(c);
    }
    ESP_LOGI(TAG, "Found %d candidates (%d free, %d paid)", (int)out.size(),
             (int)free_list.size(), (int)paid_list.size());
    if (out.empty()) ESP_LOGW(TAG, "No song found for: %s", query.c_str());
    return !out.empty();
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
    //
    // IMPORTANT: MP3 frames straddle HTTP read boundaries. When the decoder
    // returns DATA_LACK/CONTINUE it has NOT consumed the remaining bytes. Those
    // bytes must be carried over and prefixed to the next chunk. The old code
    // overwrote the buffer with fresh bytes instead, so every frame after the
    // first boundary was garbage -> endless "Not supported format".
    constexpr size_t kReadChunk = 4096;
    constexpr size_t kInBufSize = 16384;
    std::vector<uint8_t> in_buf_vec(kInBufSize);
    std::vector<uint8_t> pcm_buf_vec(32768);
    uint8_t* in_buf = in_buf_vec.data();
    uint8_t* pcm_buf = pcm_buf_vec.data();
    size_t buffered = 0;  // valid bytes at the FRONT of in_buf
    int detected_rate = 0;
    int detected_channels = 0;
    bool played_any = false;
    bool ok = true;

    // A genuinely bad track (VIP trial / m4a mislabeled as mp3) is abandoned
    // after a number of consecutive failures so we can try the next candidate.
    int consecutive_errors = 0;
    constexpr int kMaxConsecutiveErrors = 24;
    int total_errors = 0;
    int frames_decoded = 0;

    while (!cancelled_.load()) {
        // Append fresh bytes after whatever the decoder left unconsumed.
        if (kInBufSize - buffered >= 512) {
            auto got = http->Read(reinterpret_cast<char*>(in_buf + buffered),
                                  std::min(kReadChunk, kInBufSize - buffered));
            if (!got) {
                ESP_LOGE(TAG, "MP3 read failed: %s", got.error().ToString().c_str());
                ok = false;
                break;
            }
            if (*got == 0) break;  // clean EOF
            buffered += *got;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = in_buf;
        raw.len = static_cast<uint32_t>(buffered);

        int guard = 0;
        bool abort_track = false;
        while (raw.len > 0 && guard++ < 256 && !cancelled_.load()) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = pcm_buf;
            frame.len = static_cast<uint32_t>(pcm_buf_vec.size());
            esp_audio_dec_info_t info = {};
            esp_audio_err_t r = esp_mp3_dec_decode(dec, &raw, &frame, &info);

            // Advance by exactly what the decoder consumed for ANY result.
            uint32_t used = raw.consumed;
            if (used > raw.len) used = raw.len;
            if (used > 0) {
                raw.buffer += used;
                raw.len -= used;
            }

            if (r == ESP_AUDIO_ERR_DATA_LACK || r == ESP_AUDIO_ERR_CONTINUE ||
                r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                break;  // keep the tail; next HTTP chunk will be appended to it
            }
            if (r == static_cast<esp_audio_err_t>(-7)) {
                // END_OF_INFO: ID3/meta parsed, not an audio error.
                continue;
            }
            if (r != ESP_AUDIO_ERR_OK) {
                if (used == 0) {
                    // No progress at all: skip one byte so we cannot spin.
                    raw.buffer += 1;
                    raw.len -= 1;
                }
                if (++total_errors <= 4) {
                    ESP_LOGW(TAG, "MP3 decode error %d (skipping)", (int)r);
                }
                if (++consecutive_errors >= kMaxConsecutiveErrors) {
                    ESP_LOGE(TAG, "Too many decode errors (%d), aborting this track",
                             consecutive_errors);
                    ok = false;
                    abort_track = true;
                    break;
                }
                continue;
            }
            consecutive_errors = 0;  // a good frame resets the failure run
            if (frame.decoded_size == 0) continue;
            ++frames_decoded;

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
        }

        // Carry the undecoded tail forward into the next read.
        buffered = raw.len;
        if (buffered > 0 && raw.buffer != in_buf) {
            memmove(in_buf, raw.buffer, buffered);
        }
        if (abort_track) break;
        if (consecutive_errors >= kMaxConsecutiveErrors) break;
        // Defensive: if the decoder cannot progress but the buffer is full,
        // drop the oldest 1 KB to make room (should never happen for MP3).
        if (buffered >= kInBufSize) {
            memmove(in_buf, in_buf + 1024, buffered - 1024);
            buffered -= 1024;
        }
    }
    ESP_LOGI(TAG, "Decoded %d frames, %d bad bytes", frames_decoded, total_errors);

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

    std::vector<SongCandidate> candidates;
    bool success = false;
    std::string title;
    if (SearchSongs(query, candidates)) {
        // Try each candidate until one actually decodes and plays. Broken
        // links (VIP trial snippets, m4a mislabeled as mp3, partial CDN
        // responses) are skipped instead of failing the whole request.
        for (size_t i = 0; i < candidates.size() && !cancelled_.load(); ++i) {
            const auto& c = candidates[i];
            ESP_LOGI(TAG, "Trying %d/%d: %s", (int)(i + 1), (int)candidates.size(),
                     c.title.c_str());
            if (DownloadAndPlay(c.url)) {
                success = true;
                title = c.title;
                break;
            }
            ESP_LOGW(TAG, "Candidate failed, trying next: %s", c.title.c_str());
        }
    } else {
        ESP_LOGW(TAG, "No playable song found for: %s", query.c_str());
    }
    // Restore low-power WiFi after playback.
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (title.empty()) title = query;
    ESP_LOGI(TAG, "Music playback %s: %s", success ? "ok" : "failed", title.c_str());
}
