#ifndef MP3_MUSIC_PLAYER_H
#define MP3_MUSIC_PLAYER_H

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * 设备端 MP3 播放器：直接从公网 mp3 直链拉流，用 minimp3 在本机解码成 PCM，
 * 重采样后直接写 I2S 输出。不依赖电脑、不依赖局域网、不占云端音频通道。
 */
class Mp3MusicPlayer {
public:
    static Mp3MusicPlayer& GetInstance();

    // 异步开始播放，立刻返回；url 必须是 http/https
    bool PlayUrl(const std::string& url);
    // 请求停止
    void Stop();
    bool IsPlaying() const { return playing_.load(); }
    // 播放进度（毫秒），供查询工具使用
    int PositionMs() const { return position_ms_.load(); }
    int DurationMs() const { return duration_ms_.load(); }

private:
    Mp3MusicPlayer() = default;
    static void TaskEntry(void* arg);
    void Run(std::string url);

    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_req_{false};
    std::atomic<int> position_ms_{0};
    std::atomic<int> duration_ms_{0};
    TaskHandle_t task_handle_ = nullptr;
};

#endif  // MP3_MUSIC_PLAYER_H
