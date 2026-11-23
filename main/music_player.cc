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

// Find the next VALID MPEG audio frame header. We check more than the 11-bit
// sync word so we don't jump into the middle of an ID3/APE tag that happens
// to contain 0xFFE. Returns offset from buf, or buf_len if none found.
size_t FindMp3Sync(const uint8_t* buf, size_t buf_len) {
    if (buf_len < 4) return buf_len;
    for (size_t i = 0; i + 2 < buf_len; ++i) {
        if (buf[i] != 0xFF) continue;
        uint8_t b1 = buf[i + 1];
        if ((b1 & 0xE0) != 0xE0) continue;          // 11 sync bits
        if (((b1 >> 3) & 0x03) == 0x01) continue;   // version reserved
        if (((b1 >> 1) & 0x03) == 0x00) continue;   // layer reserved
        uint8_t b2 = buf[i + 2];
        int bitrate_idx = (b2 >> 4) & 0x0F;
        int srate_idx = (b2 >> 2) & 0x03;
        if (bitrate_idx == 0 || bitrate_idx == 15) continue;  // bad bitrate
        if (srate_idx == 3) continue;                          // reserved rate
        return i;
    }
    return buf_len;
}

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
    paused_.store(false);
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

void MusicPlayer::Pause() {
    if (busy_.load() && !cancelled_.load()) {
        paused_.store(true);
        ESP_LOGI(TAG, "Music paused");
    }
}

void MusicPlayer::Resume() {
    if (busy_.load()) {
        paused_.store(false);
        ESP_LOGI(TAG, "Music resumed");
    }
}

void MusicPlayer::Stop() {
    paused_.store(false);
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

    // Use the raw MP3 frame decoder (esp_mp3_dec). CONFIG_AUDIO_DECODER_MP3_SUPPORT=y
    // registers it in the decoder table, so it works without extra setup.
    // The alternative simple decoder is NOT compiled into this firmware
    // ("Decoder MP3 not registered"), so we must use the frame decoder here.
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

    // All scratch on the HEAP (stack buffers previously blew the task stack
    // and caused "BREAK instr" crashes).
    constexpr size_t kReadChunk = 4096;
    constexpr size_t kInBufSize = 16384;
    size_t out_buf_size = 16384;  // grows on BUFF_NOT_ENOUGH
    std::vector<uint8_t> in_buf_vec(kInBufSize);
    std::vector<uint8_t> out_buf_vec(out_buf_size);
    uint8_t* in_buf = in_buf_vec.data();
    size_t buffered = 0;  // valid bytes at the FRONT of in_buf
    int detected_rate = 0;
    int detected_channels = 0;
    bool played_any = false;
    bool ok = true;
    int frames_decoded = 0;
    int decode_errors = 0;
    size_t total_read = 0;
    int read_calls = 0;

    while (!cancelled_.load()) {
        // Pause: stop feeding the decoder and the speaker, but keep the HTTP
        // connection open and the undecoded bytes in `in_buf`. When resumed,
        // playback continues from exactly where it was paused.
        while (paused_.load() && !cancelled_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (cancelled_.load()) break;

        // Append fresh bytes after whatever the parser left unconsumed.
        // ATTENTION: never overwrite unconsumed bytes (corrupts the stream).
        if (kInBufSize - buffered >= 512) {
            auto got = http->Read(reinterpret_cast<char*>(in_buf + buffered),
                                  std::min(kReadChunk, kInBufSize - buffered));
            if (!got) {
                ESP_LOGE(TAG, "MP3 read failed after %u bytes: %s",
                         (unsigned)total_read, got.error().ToString().c_str());
                ok = false;
                break;
            }
            if (*got == 0) {
                ESP_LOGW(TAG, "MP3 EOF after %u bytes", (unsigned)total_read);
                break;  // clean EOF
            }
            buffered += *got;
            total_read += *got;
            if (++read_calls <= 6 || read_calls % 20 == 0) {
                ESP_LOGI(TAG, "MP3 read #%d: +%u (total %u)", read_calls,
                         (unsigned)*got, (unsigned)total_read);
            }
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = in_buf;
        raw.len = static_cast<uint32_t>(buffered);

        int guard = 0;
        bool abort_track = false;
        while (raw.len > 0 && guard++ < 256 && !cancelled_.load() && !paused_.load()) {
            uint8_t* frame_start = raw.buffer;  // where this decode attempt began
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = out_buf_vec.data();
            frame.len = static_cast<uint32_t>(out_buf_vec.size());
            esp_audio_dec_info_t info = {};
            esp_audio_err_t r = esp_mp3_dec_decode(dec, &raw, &frame, &info);

            if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH || r == ESP_AUDIO_ERR_DATA_LACK ||
                r == ESP_AUDIO_ERR_CONTINUE) {
                // Decoder needs more data; keep everything from frame_start.
                size_t keep = static_cast<size_t>(raw.buffer - frame_start) + raw.len;
                raw.buffer = frame_start;
                raw.len = static_cast<uint32_t>(keep);
                break;
            }
            if (r != ESP_AUDIO_ERR_OK) {
                // Decode failed. Search for the next valid MPEG frame header
                // starting one byte after this attempt, then resume there.
                uint8_t* search_from = frame_start + 1;
                size_t search_len = (raw.buffer + raw.len) - search_from;
                size_t off = FindMp3Sync(search_from, search_len);
                if (off < search_len) {
                    raw.buffer = search_from + off;
                    raw.len = static_cast<uint32_t>(search_len - off);
                } else {
                    raw.buffer = frame_start;
                    raw.len = 0;  // wait for more data
                }
                if (++decode_errors <= 4) {
                    ESP_LOGW(TAG, "MP3 decode error %d, resync +%u", (int)r,
                             (unsigned)off);
                }
                if (frames_decoded == 0 && decode_errors > 400) {
                    ESP_LOGE(TAG, "No frame after %d errors, aborting track",
                             decode_errors);
                    ok = false;
                    abort_track = true;
                    break;
                }
                continue;
            }
            // Success: advance by what the decoder actually consumed.
            uint32_t used = raw.consumed;
            if (used > raw.len) used = raw.len;
            if (used > 0) {
                raw.buffer += used;
                raw.len -= used;
            }
            decode_errors = 0;
            if (frame.decoded_size == 0) {
                if (used == 0 && raw.len > 0) {
                    raw.buffer += 1;
                    raw.len -= 1;
                }
                continue;
            }
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
        // Defensive: parser stalled with a full buffer -> drop oldest 1 KB.
        if (buffered >= kInBufSize) {
            memmove(in_buf, in_buf + 1024, buffered - 1024);
            buffered -= 1024;
        }
    }
    ESP_LOGI(TAG, "Decoded %d frames from %u bytes (%d reads)", frames_decoded,
             (unsigned)total_read, read_calls);

    esp_mp3_dec_close(dec);
    http->Close();
    // Success = we actually emitted a meaningful amount of audio. Skip/junk
    // errors along the way are irrelevant as long as frames kept coming.
    bool success = played_any && frames_decoded >= 20 && ok && !cancelled_.load();
    ESP_LOGI(TAG, "Playback finished, played_any=%d frames=%d ok=%d success=%d",
             played_any ? 1 : 0, frames_decoded, ok ? 1 : 0, success ? 1 : 0);
    return success;
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

    // This board has NO echo cancellation: ES8311 input_reference_=false and
    // the Lite wake engine has no AEC. Speaker audio bleeds into the mic and
    // masks "你好小智", so voice control is impossible while music plays loud.
    // Lower the output volume during playback to give the mic some headroom,
    // and restore it afterwards so TTS/alerts stay audible.
    auto* music_codec = board.GetAudioCodec();
    int saved_volume = music_codec ? music_codec->output_volume() : -1;
    constexpr int kMusicVolume = 42;
    if (music_codec != nullptr) {
        ESP_LOGI(TAG, "Lowering output volume %d -> %d for voice wake-up",
                 saved_volume, kMusicVolume);
        music_codec->SetOutputVolume(kMusicVolume);
    }

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
    // Restore low-power WiFi and speaker volume after playback.
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (music_codec != nullptr && saved_volume >= 0) {
        music_codec->SetOutputVolume(saved_volume);
        ESP_LOGI(TAG, "Restored output volume to %d", saved_volume);
    }
    if (title.empty()) title = query;
    ESP_LOGI(TAG, "Music playback %s: %s", success ? "ok" : "failed", title.c_str());
}
