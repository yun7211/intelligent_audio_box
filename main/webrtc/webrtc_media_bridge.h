/* WebRTC 媒体桥（音频 + 仅上行视频）
 *
 * 在小智**已经初始化好**的 esp_codec_dev handle（共享的 I2S codec）之上，搭起一条
 * esp_capture + av_render 管线；可选地再挂一路摄像头（DVP）视频源做仅上行视频。
 *
 * 前置条件：调用前必须先挂起助手的音频管线（AudioService::Suspend()），要视频还得先
 * 释放板载摄像头（Camera::Release()）—— 这个桥对麦克风/扬声器/摄像头是**独占**的。
 * 前置条件不满足不会报错，而是两边同时操作同一个外设，症状会很难查，所以由调用方
 * WebrtcCallService::Start() 严格按顺序保证。
 */
#ifndef WEBRTC_MEDIA_BRIDGE_H
#define WEBRTC_MEDIA_BRIDGE_H

#include <stdbool.h>
#include "esp_codec_dev.h"
#include "esp_webrtc.h"   /* esp_webrtc_media_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  在共享的 codec handle 之上构建采集+渲染管线。
 * @param  rec         录音（麦克风）的 esp_codec_dev handle
 * @param  play        播放（扬声器）的 esp_codec_dev handle
 * @param  with_video  是否同时创建摄像头 DVP 视频源（仅上行）。只有引脚已定义的板子
 *                     （lichuang-dev）会真的创建；其他板子静默保持纯音频，不算失败。
 * @return 成功返回 0，失败返回负值。
 */
int  webrtc_media_bridge_build(esp_codec_dev_handle_t rec, esp_codec_dev_handle_t play, bool with_video);

/** 用已构建好的 capture/player handle 填充 esp_webrtc_media_provider_t。 */
int  webrtc_media_bridge_get_provider(esp_webrtc_media_provider_t *out);

/** 上一次 build 是否真的创建出了视频源（请求了不等于创建成功）。 */
bool webrtc_media_bridge_has_video(void);

/** 拆掉管线。**必须在 esp_webrtc_close() 之后调用** —— 引擎还持有这两个 handle。 */
void webrtc_media_bridge_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_MEDIA_BRIDGE_H */
