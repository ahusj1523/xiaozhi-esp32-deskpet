#ifndef MUSIC_PLAYER_H_
#define MUSIC_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class Http;

// Lightweight online music player for the ES3C28P desk-pet firmware.
//
// Voice flow:
//   User says "播放晴天" -> server AI calls MCP tool self.music.play
//   -> MusicPlayer::Play("晴天") starts a background task:
//      1) GET NetEase search API (no key) -> pick first playable (fee==0) song id/title
//      2) GET outer url?id=<id>.mp3 -> MP3 byte stream
//      3) esp_mp3_dec -> PCM (usually 44100Hz stereo)
//      4) linear resample to codec output rate (e.g. 24000Hz mono)
//      5) AudioCodec::OutputData() -> speaker
//   -> restore device state to idle when done.
class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    // Non-blocking. Starts playback of the first search result for query.
    bool Play(const std::string& query);
    void Stop();
    bool IsBusy() const { return busy_.load(); }

private:
    MusicPlayer() = default;
    ~MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    static void TaskEntry(void* arg);
    void Task();

    // Returns true and fills mp3_url/title on success.
    bool SearchSong(const std::string& query, std::string& mp3_url, std::string& title);
    // Downloads mp3 from url, decodes and plays. Returns true on success.
    bool DownloadAndPlay(const std::string& mp3_url);

    // Open an HTTP(S) GET request, following up to kMaxRedirects 3xx redirects
    // manually (the HttpClient does not follow them itself, and refuses
    // https->http downgrades). On success the returned client is ready to Read.
    static constexpr int kMaxRedirects = 5;
    std::unique_ptr<Http> OpenFollowingRedirects(const std::string& url,
                                int timeout_ms, const std::string& accept_header);

    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::string query_;
    TaskHandle_t task_handle_ = nullptr;
};

#endif  // MUSIC_PLAYER_H_
