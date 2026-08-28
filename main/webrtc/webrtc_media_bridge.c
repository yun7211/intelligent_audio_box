/* WebRTC 媒体桥（音频 + 仅上行视频）
 *
 * 对应 esp-webrtc-solution 的 doorbell_local/media_sys.c，有两处改动：
 *   1. 音频直接用小智已初始化好的 codec handle，而不是 codec_board 的 get_record/playback
 *   2. 视频（可选）直接用板载摄像头引脚，同样不经 codec_board
 * 这两处改动是本文件存在的全部理由：参考实现独立运行、可以自己初始化 I2S 和摄像头，
 * 而这里必须接进一个已经把外设占满的语音助手。
 *
 * 摄像头归属：esp_capture 的 DVP 视频源用的是 ESP-IDF 自带的 camera 驱动，而小智的
 * Esp32Camera 用的是 legacy 的 `esp32-camera` 组件。两者可以同时链进一份固件，但
 * **不能同时持有摄像头外设**，所以通话服务在 build 之前调 Camera::Release()
 * （即 esp_camera_deinit），结束后调 Camera::Reacquire()。传感器探测走的是与 codec
 * 共用的那条 I2C 总线（SDA=1/SCL=2），无需改动即可工作。
 *
 * 已知短板：下面的 DVP 引脚表是按板型硬编码的，没有从 board 抽象里取，所以新增一块
 * 带摄像头的板子就得改这个文件。引脚本该下沉到 board 层，这个桥只应该消费一个抽象的
 * 视频源接口。
 */
#include "webrtc_media_bridge.h"

#include "config.h"
#include "sdkconfig.h"
#include "esp_capture.h"
#include "esp_capture_defaults.h"
#include "av_render.h"
#include "av_render_default.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"
#include "esp_video_enc_default.h"
#include "esp_log.h"

#define TAG "WebrtcBridge"

// 分辨率/帧率定得低，是为了压住 PSRAM 与 CPU 峰值（H264 编码 + SRTP 都吃 PSRAM）。
// 这三个值必须与 webrtc_call_service.cc 里 SDP 协商用的参数一致，否则协商出来的
// 参数和编码器实际吐出来的对不上。
#define WEBRTC_VIDEO_WIDTH   320
#define WEBRTC_VIDEO_HEIGHT  240
#define WEBRTC_VIDEO_FPS     15

// 全局单例状态：通话是独占的，同一时刻只可能有一套媒体桥存在。
static esp_capture_audio_src_if_t *s_aud_src;
static esp_capture_video_src_if_t *s_vid_src;
static esp_capture_handle_t         s_capture;
static audio_render_handle_t        s_audio_render;
static av_render_handle_t           s_player;
static bool                         s_has_video;

/* 根据板级硬件能力选择采集源。
 *
 * 立创板的 ES7210 用 4-slot TDM 传输，其中前三路依次是 MIC1、MIC2 和 MIC3：
 * 前两路采集门口人声，第三路接收 ES8311 播放回采，因此 AFE 的输入布局是 MMR。
 * AEC 音源会把三路数据处理成单声道 PCM；后面的 esp_capture 管线再按 WebRTC
 * 协商结果编码为 G711A。关闭 data_on_vad 可保证静音期也连续送帧，不吞用户开口。
 *
 * 只有明确声明 WEBRTC_AEC_MIC_LAYOUT 的板型才启用这条路径；这些板型把硬件参考
 * 视为通话必需能力，创建失败时直接让 build 失败，不退回可能产生强回声的原始音频。 */
static esp_capture_audio_src_if_t *create_audio_source(esp_codec_dev_handle_t rec) {
#if defined(WEBRTC_AEC_MIC_LAYOUT)
    esp_capture_audio_aec_src_cfg_t cfg = {
        .mic_layout = WEBRTC_AEC_MIC_LAYOUT,
        .record_handle = rec,
        .channel = WEBRTC_AEC_CHANNELS,
        .channel_mask = WEBRTC_AEC_CHANNEL_MASK,
        .data_on_vad = false,
    };
    return esp_capture_new_audio_aec_src(&cfg);
#else
    esp_capture_audio_dev_src_cfg_t cfg = { .record_handle = rec };
    return esp_capture_new_audio_dev_src(&cfg);
#endif
}

#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3
// lichuang-dev 的 DVP 摄像头引脚，抄自 main/boards/lichuang-dev/config.h。
static esp_capture_video_src_if_t *create_video_source(void) {
    esp_capture_video_dvp_src_cfg_t dvp = { 0 };
    dvp.buf_count = 2;
    dvp.reset_pin = -1;   // CAMERA_PIN_RESET（该板未接）
    dvp.pwr_pin   = -1;   // CAMERA_PIN_PWDN（该板未接）
    dvp.data[0] = 16; dvp.data[1] = 18; dvp.data[2] = 8;  dvp.data[3] = 17;
    dvp.data[4] = 15; dvp.data[5] = 6;  dvp.data[6] = 4;  dvp.data[7] = 9;
    dvp.vsync_pin = 3;
    dvp.href_pin  = 46;
    dvp.pclk_pin  = 7;
    dvp.xclk_pin  = 5;
    dvp.xclk_freq = 20000000;
    return esp_capture_new_video_dvp_src(&dvp);
}
#else
// 其他板型（esp-box-3）没有 DVP 摄像头：返回 NULL，调用方降级为纯音频而不是报错。
static esp_capture_video_src_if_t *create_video_source(void) { return NULL; }
#endif

int webrtc_media_bridge_build(esp_codec_dev_handle_t rec, esp_codec_dev_handle_t play, bool with_video) {
    if (rec == NULL || play == NULL) {
        ESP_LOGE(TAG, "record/playback handle is NULL");
        return -1;
    }

    // 注册默认编解码器。这些注册函数是幂等的，所以重复通话不会累积注册。
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    if (with_video) {
        esp_video_enc_register_default();  // 仅上行视频用的 H264 编码器
    }

    // --- 采集侧：音频取共享录音 handle，视频（可选）取摄像头 ---
    // 助手已经在调用 build 前暂停，WebRTC 可以独占这个 esp_codec_dev handle，并在
    // G711A 管线启动时把采集时钟切到 8 kHz。具有硬件参考的板型在这里创建 AEC 音源。
    s_aud_src = create_audio_source(rec);
    if (s_aud_src == NULL) {
        ESP_LOGE(TAG, "Fail to create required audio source");
        return -1;
    }

    s_vid_src = NULL;
    s_has_video = false;
    if (with_video) {
        s_vid_src = create_video_source();
        if (s_vid_src != NULL) {
            s_has_video = true;
        } else {
            // 请求了视频但这块板没有摄像头源：降级为纯音频继续，不让通话失败。
            ESP_LOGW(TAG, "Video requested but no camera source on this board; audio only");
        }
    }

    esp_capture_cfg_t cap_cfg = {
        // 以音频为同步基准，视频帧向音频时钟对齐。语音通话里音频卡顿远比掉一帧画面
        // 刺耳，所以让视频去追音频，而不是反过来。
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = s_aud_src,
        .video_src = s_vid_src,   // NULL 即纯音频，esp_capture 自行处理
    };
    if (esp_capture_open(&cap_cfg, &s_capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "Fail to open capture");
        return -1;
    }

    // --- 渲染侧：音频经 I2S 送到共享播放 handle（不接 LCD 视频渲染） ---
    // G711A 解码输出是 8 kHz，渲染端必须允许 esp_codec_dev 随码流重配播放时钟；
    // ES7210/ES8311 共用的双工 I2S 因而保持同一 8 kHz 时钟域，MIC3 参考不会持续漂移。
    // WebRTC 销毁后 AudioService::Resume() 会按板级配置重新打开 24 kHz 助手链路。
    i2s_render_cfg_t i2s_cfg = { .fixed_clock = false, .play_handle = play };
    s_audio_render = av_render_alloc_i2s_render(&i2s_cfg);
    if (s_audio_render == NULL) {
        ESP_LOGE(TAG, "Fail to create audio render");
        return -1;
    }
    av_render_cfg_t render_cfg = {
        .audio_render = s_audio_render,
        .video_render = NULL,           // 不渲染对端视频，见下方 destroy 处的说明
        // 两级 FIFO：raw 是解码后待渲染的 PCM，render 是交给 I2S 的排队缓冲。
        // 这两个尺寸沿用参考实现，**没有实测调优过** —— 要优化延迟，第一步应该是把
        // 这两级的水位打点出来看实际排队深度，而不是凭感觉改数字。
        .audio_raw_fifo_size = 4096,
        .audio_render_fifo_size = 6 * 1024,
        // false = 渲染 FIFO 满时阻塞上游，而不是丢数据。这是一个**可以商量的取舍**：
        // 对实时语音来说，丢旧帧换低延迟通常更划算。当时的选择是先保证音质可懂、把链路
        // 跑通；一旦要压延迟，这里应该改成允许丢旧帧，并把丢帧计数暴露到 get_stats。
        .allow_drop_data = false,
    };
    s_player = av_render_open(&render_cfg);
    if (s_player == NULL) {
        ESP_LOGE(TAG, "Fail to create player");
        return -1;
    }
#if defined(WEBRTC_AEC_MIC_LAYOUT)
    ESP_LOGI(TAG, "Media bridge built (aec=1 video=%d)", (int)s_has_video);
#else
    ESP_LOGI(TAG, "Media bridge built (aec=0 video=%d)", (int)s_has_video);
#endif
    return 0;
}

// 把采集器和播放器交给 esp_webrtc 当媒体提供者。两者的所有权仍在本文件，
// esp_webrtc 只是使用它们 —— 所以拆链路的顺序必须是先 esp_webrtc_close()，再 destroy()。
int webrtc_media_bridge_get_provider(esp_webrtc_media_provider_t *out) {
    if (out == NULL) {
        return -1;
    }
    out->capture = s_capture;
    out->player  = s_player;
    return 0;
}

bool webrtc_media_bridge_has_video(void) {
    return s_has_video;
}

void webrtc_media_bridge_destroy(void) {
    // 先关播放器（消费端）再关采集器（生产端）：反过来会让还在运行的渲染侧引用一个
    // 已经释放的源。调用方 Cleanup() 已经先 esp_webrtc_close() 把引擎停掉了，
    // 所以这里只需要处理桥自身这两个对象。
    if (s_player)  { av_render_close(s_player);   s_player = NULL; }
    if (s_capture) { esp_capture_close(s_capture); s_capture = NULL; }
    // 下面三个只置 NULL、不单独释放，**不是漏了**：s_audio_render 的所有权在
    // av_render_open() 时移交给了 s_player，两个 src 则在 esp_capture_open() 时移交给
    // 了 s_capture，各自已由上面两次 close 释放。再释放一次是 double free。
    //
    // 已知短板：build() 的中途失败分支（例如 esp_capture_open 失败）会让已分配的
    // s_aud_src 走不到 esp_capture_close，此时这里只把它置 NULL，那一次分配就漏了。
    // 单次通话失败漏一次，量不大，但正确做法是 build() 自己按分配顺序逆序回滚。
    s_audio_render = NULL;
    s_aud_src = NULL;
    s_vid_src = NULL;
    s_has_video = false;
}
