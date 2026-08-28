#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <thread>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "esp_camera.h"
#include "jpg/image_to_jpeg.h"

struct JpegChunk
{
    uint8_t *data;
    size_t len;
};

/* ESP32 摄像头实现：管理驱动 frame buffer，并在后台线程把原始帧编码为 JPEG 供显示/上传。 */
class Esp32Camera : public Camera
{
private:
    bool streaming_on_ = false;
    bool swap_bytes_enabled_ = true;  // RGB565 默认交换高低字节以匹配编码器
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    camera_fb_t *current_fb_ = nullptr;
    uint8_t *encode_buf_ = nullptr;  // JPEG 编码输入缓冲，可在此完成字节交换
    size_t encode_buf_size_ = 0;
    camera_config_t config_ = {};    // 保留配置，WebRTC 通话结束后用于重新初始化

public:
    Esp32Camera(const camera_config_t &config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string &url, const std::string &token) override;
    virtual bool Capture() override;
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual bool SetSwapBytes(bool enabled) override;
    virtual std::string Explain(const std::string &question) override;

    // Release/reacquire the esp32-camera driver so the WebRTC video path can
    // exclusively own the camera peripheral during a call.
    void Release() override;
    void Reacquire() override;
};
