#pragma once

#include "../lvgl_image.h"
#include "gifdec.h"
#include <lvgl.h>
#include <memory>
#include <functional>

/**
 * LVGL GIF 动画包装层：gifdec 负责逐帧解码，LVGL 定时器负责按帧延时推进画面。
 * 对象持有解码器、ARGB8888 画布和定时器，析构时统一清理。
 */
class LvglGif {
public:
    explicit LvglGif(const lv_img_dsc_t* img_dsc);
    virtual ~LvglGif();

    // LvglImage interface implementation
    virtual const lv_img_dsc_t* image_dsc() const;

    /**
     * Start/restart GIF animation
     */
    void Start();

    /**
     * Pause GIF animation
     */
    void Pause();

    /**
     * Resume GIF animation
     */
    void Resume();

    /**
     * Stop GIF animation and rewind to first frame
     */
    void Stop();

    /**
     * Check if GIF is currently playing
     */
    bool IsPlaying() const;

    /**
     * Check if GIF was loaded successfully
     */
    bool IsLoaded() const;

    /**
     * Get loop count
     */
    int32_t GetLoopCount() const;

    /**
     * Set loop count
     */
    void SetLoopCount(int32_t count);

    /**
     * Get loop delay in milliseconds (delay between loops)
     */
    uint32_t GetLoopDelay() const;

    /**
     * Set loop delay in milliseconds (delay between loops)
     * @param delay_ms Delay in milliseconds before starting next loop. 0 means no delay.
     */
    void SetLoopDelay(uint32_t delay_ms);

    /**
     * Get GIF dimensions
     */
    uint16_t width() const;
    uint16_t height() const;

    /**
     * Set frame update callback
     */
    void SetFrameCallback(std::function<void()> callback);

private:
    // GIF 解码上下文和输出画布。
    gd_GIF* gif_;
    
    // 指向解码器画布的 LVGL 图片描述符，不额外复制像素。
    lv_img_dsc_t img_dsc_;
    
    // 动画时钟根据 GIF 帧延迟安排下一次解码。
    lv_timer_t* timer_;
    
    // Last frame update time
    uint32_t last_call_;
    
    // 播放状态。
    bool playing_;
    bool loaded_;
    
    // 无限循环 GIF 每轮结束后的可选停顿。
    uint32_t loop_delay_ms_;      // Delay between loops in milliseconds
    bool loop_waiting_;           // Whether we're waiting for the next loop
    uint32_t loop_wait_start_;    // Timestamp when loop wait started
    
    // Frame update callback
    std::function<void()> frame_callback_;
    
    /**
     * Update to next frame
     */
    void NextFrame();
    
    /**
     * Cleanup resources
     */
    void Cleanup();
};
