#ifndef HTTP_MUSIC_PLAYER_H
#define HTTP_MUSIC_PLAYER_H

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * 从网络地址拉取 ogg/opus 音频流，解封装后塞进 AudioService 的解码队列播放。
 * 只负责"给地址就播"，不依赖云端音频通道。
 */
class HttpMusicPlayer {
public:
    static HttpMusicPlayer& GetInstance();

    // 异步开始播放，立刻返回；url 必须是 http/https
    bool PlayUrl(const std::string& url);
    // 请求停止（会等当前读流循环退出）
    void Stop();
    bool IsPlaying() const { return playing_.load(); }

private:
    HttpMusicPlayer() = default;
    static void TaskEntry(void* arg);
    void Run(std::string url);

    std::atomic<bool> playing_{false};
    std::atomic<bool> stop_req_{false};
    TaskHandle_t task_handle_ = nullptr;
};

#endif  // HTTP_MUSIC_PLAYER_H
