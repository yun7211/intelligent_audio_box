#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

// 音频前处理抽象：实现可包含 AEC、降噪和 VAD，统一输出适合 Opus 编码的单声道 PCM。
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    virtual void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
    // 播报期间的打断检测模式：同时运行 AEC 与 VAD，但是否上传 PCM 由 AudioService 决定。
    // 没有参考通道或不支持 AFE 的实现返回 false。
    virtual bool EnableBargeInDetection(bool enable) = 0;
};

#endif
