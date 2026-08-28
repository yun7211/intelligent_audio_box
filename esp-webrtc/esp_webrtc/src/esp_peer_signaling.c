/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "esp_peer_signaling.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * 信令层与 esp_peer 的职责不同：它只负责通过 HTTP/WebSocket 等外部通道交换 SDP、ICE、
 * BYE 和自定义消息，不处理媒体包。与 esp_peer.c 相同，这里采用“操作表 + 私有句柄”的
 * 包装模式，使 AppRTC、本地 HTTP 等信令实现都能接到统一 API 后面。
 */

typedef struct {
    esp_peer_signaling_handle_t sig_handle;
    esp_peer_signaling_impl_t   impl;
} signaling_wrapper_t;

int esp_peer_signaling_start(esp_peer_signaling_cfg_t *cfg, const esp_peer_signaling_impl_t *impl, esp_peer_signaling_handle_t *handle)
{
    // start 成功后才把包装器交给调用方；失败路径立即释放，不留下半初始化句柄。
    if (cfg == NULL || impl == NULL || handle == NULL || impl->start == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    signaling_wrapper_t *sig = calloc(1, sizeof(signaling_wrapper_t));
    if (sig == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    int ret = impl->start(cfg, &sig->sig_handle);
    if (ret != ESP_PEER_ERR_NONE) {
        free(sig);
        return ret;
    }
    sig->impl = *impl;
    *handle = sig;
    return ret;
}

int esp_peer_signaling_send_msg(esp_peer_signaling_handle_t handle, esp_peer_signaling_msg_t *msg)
{
    // 上层给出统一消息类型，具体实现负责序列化成目标信令服务所需格式。
    if (handle == NULL || msg == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    signaling_wrapper_t *sig = (signaling_wrapper_t *)handle;
    return sig->impl.send_msg(sig->sig_handle, msg);
}

int esp_peer_signaling_stop(esp_peer_signaling_handle_t handle)
{
    // 先停止具体信令连接，再释放只属于本层的包装器。
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    signaling_wrapper_t *sig = (signaling_wrapper_t *)handle;
    int ret = sig->impl.stop(sig->sig_handle);
    free(sig);
    return ret;
}
