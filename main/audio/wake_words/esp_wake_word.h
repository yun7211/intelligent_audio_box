#ifndef ESP_WAKE_WORD_H
#define ESP_WAKE_WORD_H

#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

#include "audio_codec.h"
#include "wake_word.h"

// 直接调用 WakeNet 的轻量唤醒词实现，不经过完整 AFE 前处理链路。
// 适用于资源较紧张或硬件侧已完成必要音频处理的板型。
class EspWakeWord : public WakeWord {
public:
    EspWakeWord();
    ~EspWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list);
    void Feed(const std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    void Start();
    void Stop();
    size_t GetFeedSize();
    void EncodeWakeWordData();
    bool GetWakeWordOpus(std::vector<uint8_t>& opus);
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

private:
    esp_wn_iface_t *wakenet_iface_ = nullptr;
    model_iface_data_t *wakenet_data_ = nullptr;
    srmodel_list_t *wakenet_model_ = nullptr;
    AudioCodec* codec_ = nullptr;
    std::atomic<bool> running_ = false;

    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::string last_detected_wake_word_;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
};

#endif
