/* WebRTC 本地 HTTPS 信令
 * 移植自 esp-webrtc-solution 的 doorbell_local/webrtc_http_server.h。
 */
#pragma once

#include "esp_err.h"
#include "esp_peer_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  取设备自托管的 HTTPS 信令实现。
 *         把返回值填给 esp_webrtc_cfg_t.signaling_impl，即可做局域网内的直连通话
 *         （不经任何外部信令服务器，手机浏览器直接连设备 IP）。
 *
 * @return
 *       - NULL    内存不足
 *       - 其他    HTTPS 服务器信令实现。返回的是**静态单例**，调用方不要释放。
 */
const esp_peer_signaling_impl_t *esp_signaling_get_http_impl(void);

#ifdef __cplusplus
}
#endif
