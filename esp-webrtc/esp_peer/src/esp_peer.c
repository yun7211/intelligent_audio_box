/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "esp_peer.h"
#include <stdlib.h>
#include <string.h>
#include "dtls_srtp.h"

#define WEAK __attribute__((weak))

/*
 * esp_peer 是 PeerConnection 的统一门面，本文件本身不实现 ICE、DTLS、SRTP 或 SCTP。
 * 它把调用转发给 esp_peer_ops_t 中选定的具体实现，并用 peer_wrapper_t 同时保存“操作表”
 * 和“实现私有句柄”。上层因此只依赖稳定的 esp_peer_* API，可替换底层 PeerConnection。
 *
 * 生命周期：esp_peer_open() 分配包装器并调用实现层 open；中间 API 只校验参数后转发；
 * esp_peer_close() 先让实现层释放私有资源，最后释放包装器自身。
 */

typedef struct {
    esp_peer_ops_t    ops;
    esp_peer_handle_t handle;
} peer_wrapper_t;

int esp_peer_open(esp_peer_cfg_t *cfg, const esp_peer_ops_t *ops, esp_peer_handle_t *handle)
{
    // 复制操作表而不是保存外部指针，保证调用期间 ops 的生命周期独立于调用者栈变量。
    if (cfg == NULL || ops == NULL || handle == NULL || ops->open == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = calloc(1, sizeof(peer_wrapper_t));
    if (peer == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    memcpy(&peer->ops, ops, sizeof(esp_peer_ops_t));
    int ret = ops->open(cfg, &peer->handle);
    if (ret != ESP_PEER_ERR_NONE) {
        free(peer);
        return ret;
    }
    *handle = peer;
    return ret;
}

int esp_peer_new_connection(esp_peer_handle_t handle)
{
    // 开始一次新的 SDP/ICE 协商；真正的 offer/answer 生成由具体实现完成。
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.new_connection) {
        return peer->ops.new_connection(peer->handle);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_create_data_channel(esp_peer_handle_t handle, esp_peer_data_channel_cfg_t *ch_cfg)
{
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.create_data_channel) {
        return peer->ops.create_data_channel(peer->handle, ch_cfg);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_close_data_channel(esp_peer_handle_t handle, const char *label)
{
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.close_data_channel) {
        return peer->ops.close_data_channel(peer->handle, label);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_update_ice_info(esp_peer_handle_t handle, esp_peer_role_t role, esp_peer_ice_server_cfg_t* server, int server_num)
{
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.update_ice_info) {
        return peer->ops.update_ice_info(peer->handle, role, server, server_num);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_send_msg(esp_peer_handle_t handle, esp_peer_msg_t *msg)
{
    // 将信令层收到的远端 SDP 或 ICE candidate 交给 PeerConnection 状态机。
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.send_msg) {
        return peer->ops.send_msg(peer->handle, msg);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_send_video(esp_peer_handle_t handle, esp_peer_video_frame_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.send_video) {
        return peer->ops.send_video(peer->handle, info);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_send_audio(esp_peer_handle_t handle, esp_peer_audio_frame_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.send_audio) {
        return peer->ops.send_audio(peer->handle, info);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_send_data(esp_peer_handle_t handle, esp_peer_data_frame_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.send_data) {
        return peer->ops.send_data(peer->handle, info);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_set_rtp_transformer(esp_peer_handle_t handle, esp_peer_rtp_transform_role_t role,
                                 esp_peer_rtp_transform_cb_t *transform_cb, void *ctx)
{
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.set_rtp_transformer) {
        return peer->ops.set_rtp_transformer(peer->handle, role, transform_cb, ctx);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_main_loop(esp_peer_handle_t handle)
{
    // 驱动底层状态机、超时和收发处理；调用方通常在独立任务中周期执行。
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.main_loop) {
        return peer->ops.main_loop(peer->handle);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_disconnect(esp_peer_handle_t handle)
{
    // 只结束当前连接，包装器仍可用于下一次 new_connection()。
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.disconnect) {
        return peer->ops.disconnect(peer->handle);
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_query(esp_peer_handle_t handle)
{
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    if (peer->ops.query) {
        peer->ops.query(peer->handle);
        return ESP_PEER_ERR_NONE;
    }
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_close(esp_peer_handle_t handle)
{
    // close() 后无论实现层是否支持关闭，都必须释放本层包装器。
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    int ret = ESP_PEER_ERR_NOT_SUPPORT;
    if (peer->ops.close) {
        ret = peer->ops.close(peer->handle);
    }
    free(peer);
    return ret;
}

/**
 * @brief 以下能力可能只有默认 PeerConnection 实现支持。
 *        提供弱符号兜底，替换成其他实现时即使没有该扩展也能正常链接。
 */
int WEAK peer_default_get_paired_addr(esp_peer_handle_t handle, esp_peer_addr_t *addr)
{
    return ESP_PEER_ERR_NOT_SUPPORT;
}

int esp_peer_get_paired_addr(esp_peer_handle_t handle, esp_peer_addr_t *addr)
{
    if (handle == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_wrapper_t *peer = (peer_wrapper_t *)handle;
    return peer_default_get_paired_addr(peer->handle, addr);
}

int esp_peer_pre_generate_cert(void)
{
    // 提前生成并缓存 DTLS 自签名证书，把首次建连时的耗时挪到业务空闲阶段。
    int ret = dtls_srtp_gen_cert();
    return ret == 0 ? ESP_PEER_ERR_NONE : ESP_PEER_ERR_FAIL;
}

