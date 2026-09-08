#ifndef CUSTOM_WAKE_WORD_H
#define CUSTOM_WAKE_WORD_H

#include <esp_attr.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <model_path.h>

#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "audio_codec.h"
#include "wake_word.h"

// 基于 MultiNet 命令词模型的自定义唤醒实现：从模型配置加载命令、阈值和有效持续时间。
// Feed() 累积模型要求的一帧 PCM，命中后保存触发词，并保留可编码上传的唤醒音频。
class CustomWakeWord : public WakeWord {
public:
    CustomWakeWord();
    ~CustomWakeWord();

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
    struct Command {
        std::string command;
        std::string text;
        std::string action;
    };

    // MultiNet 推理接口、模型实例及从资源配置解析出的命令表。
    esp_mn_iface_t* multinet_ = nullptr;
    model_iface_data_t* multinet_model_data_ = nullptr;
    srmodel_list_t *models_ = nullptr;
    char* mn_name_ = nullptr;
    std::string language_ = "cn";
    int duration_ = 3000;
    float threshold_ = 0.2;
    std::deque<Command> commands_;
 
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    std::atomic<bool> running_ = false;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    std::deque<std::vector<int16_t>> wake_word_pcm_;
    // 前滚窗口已缓存样本总数，用于按实际样本数裁剪窗口（16 kHz × 2 秒），
    // 不依赖"一块约 30 ms"的写死假设。
    size_t wake_word_samples_ = 0;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    // PCM 生产者与 Opus 编码任务通过队列和条件变量解耦，避免阻塞实时检测。
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    void StoreWakeWordData(const std::vector<int16_t>& data);
    void ParseWakenetModelConfig();
};

#endif
