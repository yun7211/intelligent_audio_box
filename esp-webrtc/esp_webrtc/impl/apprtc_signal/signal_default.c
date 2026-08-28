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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"
#include "media_lib_os.h"
#include "esp_peer_signaling.h"
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include "esp_netif.h"
#include <sdkconfig.h>
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_websocket_client.h"
#include "esp_tls.h"

#include <cJSON.h>

#include "https_client.h"

#define TAG "APPRTC_SIG"

/*
 * AppRTC 信令分两阶段建立：
 * 1. HTTP POST 加入房间，取得 client_id、WebSocket 地址、历史消息和 ICE 凭据接口；
 * 2. 建立 WebSocket 并注册房间成员，随后实时接收 SDP/candidate/BYE/customized。
 *
 * SDP/candidate 的发送仍使用房间 HTTP 接口，WebSocket 主要承载服务端推送。TURN 凭据是
 * 短期凭据，后台 reload_ice_task 会在到期前刷新并通知上层更新 PeerConnection。
 */

typedef struct {
    char  *client_id;
    bool   is_initiator;
    char  *wss_url;
    char  *room_link;
    char  *wss_post_url;
    char  *room_id;
    char  *ice_server;
    cJSON *msg_json;
    cJSON *root;
    char  *base_url;
} client_info_t;

typedef struct {
    esp_websocket_client_handle_t ws;
    int                           connected;
} wss_client_t;

typedef struct {
    client_info_t                 client_info;
    esp_peer_signaling_ice_info_t ice_info;
    wss_client_t                 *wss_client;
    esp_peer_signaling_cfg_t      cfg;
    uint32_t                      ice_expire_time;
    bool                          ice_reloading;
    bool                          ice_reload_stop;
} wss_sig_t;

static int wss_signal_stop(esp_peer_signaling_handle_t sig);

static void free_client_info(client_info_t *info)
{
    // 多数字符串指向 root JSON 内存，删除 root 即统一释放；base_url 是单独 strdup 的。
    if (info->root) {
        cJSON_Delete(info->root);
    }
    if (info->base_url) {
        free(info->base_url);
        info->base_url = NULL;
    }
    memset(info, 0, sizeof(client_info_t));
}

static void free_ice_info(esp_peer_signaling_ice_info_t *info)
{
    // ICE URL/用户名/密码由 credential_body() 独立复制，需要逐项释放。
    if (info->server_info.stun_url) {
        free(info->server_info.stun_url);
    }
    if (info->server_info.user) {
        free(info->server_info.user);
    }
    if (info->server_info.psw) {
        free(info->server_info.psw);
    }
    memset(info, 0, sizeof(esp_peer_signaling_ice_info_t));
}

static void get_client_info(http_resp_t *resp, void *ctx)
{
    // 解析“加入房间”响应。字段字符串由 root 持有，因此把 root 一并保存在 client_info 中。
    const char *data = (const char *)resp->data;
    cJSON *root = cJSON_Parse((const char *)data);
    if (!root) {
        return;
    }
    client_info_t *info = (client_info_t *)ctx;
    cJSON *result_json = cJSON_GetObjectItem(root, "result");
    cJSON *params_json = cJSON_GetObjectItem(root, "params");
    if (result_json) {
        ESP_LOGI(TAG, "result %s", result_json->valuestring);
    } else {
        cJSON_Delete(root);
        return;
    }

    if (params_json) {
        cJSON *item = cJSON_GetObjectItem(params_json, "client_id");
        if (item) {
            info->client_id = item->valuestring;
        }
        item = cJSON_GetObjectItem(params_json, "is_initiator");
        if (item) {
            info->is_initiator = (strcmp(item->valuestring, "true") == 0);
        }
        item = cJSON_GetObjectItem(params_json, "wss_post_url");
        if (item) {
            info->wss_post_url = item->valuestring;
        }
        item = cJSON_GetObjectItem(params_json, "room_id");
        if (item) {
            info->room_id = item->valuestring;
        }
        item = cJSON_GetObjectItem(params_json, "wss_url");
        if (item) {
            info->wss_url = item->valuestring;
        }
        item = cJSON_GetObjectItem(params_json, "ice_server_url");
        if (item) {
            info->ice_server = item->valuestring;
        }
        item = cJSON_GetObjectItem(params_json, "messages");
        if (item) {
            info->msg_json = item;
        }
        info->root = root;
        printf("Initials set to %d\n", info->is_initiator);
        return;
    }
    cJSON_Delete(root);
}

static void credential_body(http_resp_t *resp, void *ctx)
{
    // 解析 TURN/STUN 临时凭据；当前实现只采用服务端返回的第一组 URL。
    const char *data = (const char *)resp->data;
    cJSON *root = cJSON_Parse((const char *)data);
    if (!root) {
        return;
    }
    esp_peer_signaling_ice_info_t *ice = (esp_peer_signaling_ice_info_t *)ctx;
    do {
        cJSON *ice_servers = cJSON_GetObjectItem(root, "iceServers");
        if (ice_servers == NULL) {
            break;
        }
        cJSON *server_arr = cJSON_GetArrayItem(ice_servers, 0);
        if (server_arr == NULL) {
            break;
        }
        cJSON *url_arr = cJSON_GetObjectItem(server_arr, "urls");
        if (url_arr == NULL) {
            break;
        }
        cJSON *urls = cJSON_GetArrayItem(url_arr, 0);
        if (urls == NULL) {
            break;
        }
        // 当前只取第一条 ICE URL；多服务器容灾尚未在此实现。
        ice->server_info.stun_url = strdup(urls->valuestring);
        if (ice->server_info.stun_url == NULL) {
            break;
        }
        cJSON *user = cJSON_GetObjectItem(server_arr, "username");
        if (user == NULL) {
            break;
        }
        ice->server_info.user = strdup(user->valuestring);
        cJSON *psw = cJSON_GetObjectItem(server_arr, "credential");
        if (psw == NULL) {
            break;
        }
        ice->server_info.psw = strdup(psw->valuestring);
        // 上游这里原本 printf 出完整的 TURN 用户名和密码。那是一份**可直接复用的中继
        // 凭证**，而串口日志会被转贴到 issue、聊天和录屏里。只报"拿到了"，不报是什么。
        printf("Got ICE server credentials\n");
    } while (0);
    cJSON_Delete(root);
}

static void destroy_wss(wss_client_t *wss)
{
    if (wss->ws) {
        esp_websocket_client_stop(wss->ws);
        esp_websocket_client_destroy(wss->ws);
    }
    free(wss);
}

static int send_register(wss_client_t *wss, client_info_t *info)
{
    // WebSocket 建立后先注册 room_id/client_id，服务端才会把房间消息推给本连接。
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "cmd", "register");
    cJSON_AddStringToObject(json, "roomid", info->room_id);
    cJSON_AddStringToObject(json, "clientid", info->client_id);
    int ret = 0;
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        ESP_LOGI(TAG, "send to remote : %s", payload);
        ret = esp_websocket_client_send_text(wss->ws, payload, strlen(payload), portMAX_DELAY);
        free(payload);
    }
    return ret;
}

static int send_bye(wss_client_t *wss, client_info_t *info)
{
    // BYE 通过 WebSocket 发给对端，使其立即结束当前 PeerConnection。
    cJSON *json = cJSON_CreateObject();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "cmd", "send");
    cJSON_AddStringToObject(msg, "type", "bye");
    char *msg_body = cJSON_PrintUnformatted(msg);
    if (msg_body) {
        cJSON_AddStringToObject(json, "msg", msg_body);
    }
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    cJSON_Delete(msg);
    int ret = 0;
    if (payload) {
        ESP_LOGI(TAG, "send to remote : %s", payload);
        ret = esp_websocket_client_send_text(wss->ws, payload, strlen(payload), portMAX_DELAY);
        free(payload);
    }
    free(msg_body);
    return ret > 0 ? 0 : -1;
}

/* WebSocket 连接成功：注册房间身份，并通知高层信令已可用。 */
static int on_connect(void *user)
{
    wss_sig_t *sg = user;
    wss_client_t *wss = sg->wss_client;
    wss->connected = true;
    send_register(wss, &sg->client_info);
    if (sg->cfg.on_connected) {
        sg->cfg.on_connected(sg->cfg.ctx);
    }
    return 0;
}

/*
 * WebSocket 文本消息入口。AppRTC 的 msg 字段本身仍是 JSON 字符串，因此存在“JSON 套 JSON”
 * 的两层解析；解析后统一转换为 esp_peer_signaling_msg_t 交给 esp_webrtc。
 */
static int on_text(void *user, const char *text, size_t len)
{
    if (len == 0) {
        return 0;
    }
    // printf("on_text(user, ws, '%.*s', %zd)\n", (int) len, text, len);
    wss_sig_t *sg = user;
    if (sg->cfg.on_msg) {
        cJSON *_json = cJSON_Parse(text);
        cJSON *msg = NULL;
        do {
            if (_json == NULL) {
                break;
            }
            msg = cJSON_GetObjectItem(_json, "msg");
            // AppRTC 常把真正消息序列化为外层 JSON 的 msg 字符串，需要再解析一次。
            cJSON *method;
            if (msg == NULL) {
                method = cJSON_GetObjectItem(_json, "type");
                if (method == NULL) {
                    break;
                }
                msg = _json;
            } else {
                msg = cJSON_Parse(msg->valuestring);
                method = cJSON_GetObjectItem(msg, "type");
                if (method == NULL) {
                    break;
                }
            }
            if (strcmp(method->valuestring, "offer") == 0) {
                cJSON *sdp = cJSON_GetObjectItem(msg, "sdp");
                if (sdp) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_SDP,
                        .data = (uint8_t *)sdp->valuestring,
                        .size = strlen(sdp->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                }
            } else if (strcmp(method->valuestring, "answer") == 0) {
                cJSON *sdp = cJSON_GetObjectItem(msg, "sdp");
                if (sdp) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_SDP,
                        .data = (uint8_t *)sdp->valuestring,
                        .size = strlen(sdp->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                }
            } else if (strcmp(method->valuestring, "bye") == 0) {
                // 对端离开，通知上层拆除当前连接。
                esp_peer_signaling_msg_t msg = {
                    .type = ESP_PEER_SIGNALING_MSG_BYE,
                };
                sg->cfg.on_msg(&msg, sg->cfg.ctx);
                // 对端离开后本端转为下一轮协商的发起方（controlling）。
                ESP_LOGI(TAG, "Peer leaved become controlling now");
                sg->ice_info.is_initiator = true;
            } else if (strcmp(method->valuestring, "candidate") == 0) {
                cJSON *candidate = cJSON_GetObjectItem(msg, "candidate");
                if (candidate) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_CANDIDATE,
                        .data = (uint8_t *)candidate->valuestring,
                        .size = strlen(candidate->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                }
            } else if (strcmp(method->valuestring, "customized") == 0) {
                cJSON *custom_data = cJSON_GetObjectItem(msg, "data");
                if (custom_data) {
                    esp_peer_signaling_msg_t msg = {
                        .type = ESP_PEER_SIGNALING_MSG_CUSTOMIZED,
                        .data = (uint8_t *)custom_data->valuestring,
                        .size = strlen(custom_data->valuestring),
                    };
                    sg->cfg.on_msg(&msg, sg->cfg.ctx);
                }
            }
        } while (0);
        if (msg == NULL) {
            ESP_LOGE(TAG, "Bad json input");
        } else if (msg != _json) {
            cJSON_Delete(msg);
        }
        cJSON_Delete(_json);
    }
    return 0;
}

/* WebSocket 关闭：把信令断开事件上报，上层据此终止当前 PeerConnection。 */
static int on_close(void *user, int code)
{
    printf("on_close code %d\n", code);
    wss_sig_t *sg = (wss_sig_t *)user;
    if (sg->cfg.on_close) {
        sg->cfg.on_close(sg->cfg.ctx);
    }
    return 0;
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void websocket_event_handler(void *ctx, esp_event_base_t base, int32_t event_id, void *event_data)
{
    // ESP WebSocket 事件只做适配，具体的注册、解析和关闭逻辑分别收敛到 on_* 函数。
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            on_connect(ctx);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            on_close(ctx, (int)data->error_handle.esp_ws_handshake_status_code);
            log_error_if_nonzero("HTTP status code", data->error_handle.esp_ws_handshake_status_code);
            if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", data->error_handle.esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno", data->error_handle.esp_transport_sock_errno);
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            on_text(ctx, data->data_ptr, data->data_len);
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
            log_error_if_nonzero("HTTP status code", data->error_handle.esp_ws_handshake_status_code);
            if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", data->error_handle.esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno", data->error_handle.esp_transport_sock_errno);
            }
            break;
    }
}

int create_wss(wss_sig_t *sg)
{
    // Origin 必须与房间服务地址一致，否则 AppRTC WebSocket 服务会拒绝注册。
    wss_client_t *wss = calloc(1, sizeof(wss_client_t));
    if (wss == NULL) {
        return -1;
    }
    char origin[128];
    snprintf(origin, 128, "Origin: %s\r\n", sg->client_info.base_url);

    esp_websocket_client_config_t ws_cfg = {
        .uri = sg->client_info.wss_url,
        .headers = origin,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        // .keep_alive_enable = true,
        .reconnect_timeout_ms = 60 * 1000,
        // .ping_interval_sec = 30,
        .network_timeout_ms = 10000,
        // .keep_alive_interval = 60,
        .buffer_size = 20 * 1024,
    };
    ESP_LOGI(TAG, "Connecting to %s...", ws_cfg.uri);
    wss->ws = esp_websocket_client_init(&ws_cfg);
    do {
        if (wss->ws == NULL) {
            break;
        }
        esp_websocket_register_events(wss->ws, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)sg);
        int ret = esp_websocket_client_start(wss->ws);
        if (ret != 0) {
            break;
        }
        sg->wss_client = wss;
        return ret;
    } while (0);
    return -1;
}

static void wss_send_offer_msg(wss_sig_t *sg)
{
    // 被叫加入房间时，HTTP 响应可能已携带对端先前发送的 offer/candidate，按原顺序重放。
    cJSON *msg = sg->client_info.msg_json;
    if (msg == NULL) {
        return;
    }
    int n = cJSON_GetArraySize(msg);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(msg, i);
        if (item == NULL || item->valuestring == NULL) {
            continue;
        }
        char *msg_body = item->valuestring;
        ESP_LOGD(TAG, "Send initial sdp message");
        on_text(sg, msg_body, strlen(msg_body));
    }
}

static char *get_base_url(char *room_url)
{
    char *url = strdup(room_url);
    if (url) {
        char *sep = strstr(url, "/join");
        if (sep) {
            *sep = 0;
        }
    }
    return url;
}

static void update_ice_expire_time(wss_sig_t *sg)
{
    // TURN 用户名中携带 Unix 到期时间；提前 60 秒刷新，无法解析时每 5 分钟刷新一次。
    uint32_t expire_time = atoi(sg->ice_info.server_info.user);
    uint32_t cur = time(NULL);
    if (expire_time > cur && expire_time < cur + 3600) {
        sg->ice_expire_time = expire_time - cur;
        if (sg->ice_expire_time > 60) {
            sg->ice_expire_time -= 60;
        }
    } else {
        // 凭据未给出可信到期时间时，每 5 分钟保守刷新。
        sg->ice_expire_time = 300;
    }
}

static void reload_ice_task(void *arg)
{
    // 后台刷新临时 ICE 凭据，不阻塞 WebSocket 收消息和 PeerConnection 主循环。
    wss_sig_t *sg = (wss_sig_t *)arg;
    uint32_t start = time(NULL);
    update_ice_expire_time(sg);
    while (!sg->ice_reload_stop) {
        media_lib_thread_sleep(50);
        uint32_t cur = time(NULL);
        if (cur > start + sg->ice_expire_time) {
            int ret = https_post(sg->client_info.ice_server, NULL, NULL, NULL, credential_body, &sg->ice_info);
            if (ret != 0) {
                break;
            }
            // 同时发送空业务消息维持房间活跃，避免长期通话被服务端清理。
            char *heart_beat = "{\"cmd\":\"send\",\"msg\":\"{\\\"error\\\":\\\"\\\"}\"}";
            esp_websocket_client_send_text(sg->wss_client->ws, heart_beat, strlen(heart_beat), portMAX_DELAY);
            update_ice_expire_time(sg);
            if (sg->cfg.on_ice_info) {
                sg->cfg.on_ice_info(&sg->ice_info, sg->cfg.ctx);
            }
            start = cur;
        }
    }
    sg->ice_reload_stop = false;
    sg->ice_reloading = false;
    media_lib_thread_destroy(NULL);
}

static int stop_reload_ice_timer(wss_sig_t *sg)
{
    sg->ice_reload_stop = true;
    while (sg->ice_reloading) {
        media_lib_thread_sleep(50);
    }
    return 0;
}

static int start_reload_ice_timer(wss_sig_t *sg)
{
    media_lib_thread_handle_t handle;
    media_lib_thread_create_from_scheduler(&handle, "ice_reload", reload_ice_task, sg);
    if (handle) {
        sg->ice_reloading = true;
        return 0;
    }
    return -1;
}

static int wss_signal_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h)
{
    // 启动顺序：加入房间 -> 取 ICE 凭据 -> 回调 ICE 信息 -> 连 WebSocket -> 启动刷新任务。
    if (cfg->signal_url == NULL || cfg == NULL || h == NULL) {
        return -1;
    }
    wss_sig_t *sg = calloc(1, sizeof(wss_sig_t));
    if (sg == NULL) {
        return -1;
    }
    int ret = 0;
    // 必须先通过 HTTP 加入房间，才能得到后续 WebSocket 注册所需的 client_id。
    https_post(cfg->signal_url, NULL, NULL, NULL, get_client_info, &sg->client_info);
    if (sg->client_info.client_id == NULL) {
        // 不打印 signal_url：房间号拼在它末尾，而房间号是"谁能和这台设备通话"的唯一
        // 门槛（远程开门就靠通话建立）。失败原因本身已经够定位问题了。
        ESP_LOGE(TAG, "Fail to join signaling room (no client id)");
        ret = -1;
        goto __exit;
    }
    sg->client_info.base_url = get_base_url(cfg->signal_url);
    if (sg->client_info.base_url == NULL) {
        ret = -1;
        goto __exit;
    }
    if (sg->client_info.ice_server == NULL) {
        ESP_LOGE(TAG, "Fail to get ICE server url from the join response");
        ret = -1;
        goto __exit;
    }
    https_post(sg->client_info.ice_server, NULL, NULL, NULL, credential_body, &sg->ice_info);
    sg->ice_info.is_initiator = sg->client_info.is_initiator;
    if (sg->ice_info.server_info.psw == NULL) {
        printf("Fail to get password\n");
        ret = -1;
        goto __exit;
    }
    // 配置中包含上层回调和 ctx，浅拷贝后生命周期与信令对象一致。
    sg->cfg = *cfg;
    if (sg->cfg.on_ice_info) {
        sg->cfg.on_ice_info(&sg->ice_info, sg->cfg.ctx);
    }
    ESP_LOGI(TAG, "Registering signaling channel.");
    *h = sg;
    ret = create_wss(sg);
    if (ret == 0) {
        start_reload_ice_timer(sg);
        wss_send_offer_msg(sg);
        return ret;
    }
__exit:
    *h = NULL;
    wss_signal_stop(sg);
    return ret;
}

static int send_customized_data(wss_client_t *wss, esp_peer_signaling_msg_t *msg)
{
    if (msg->data[msg->size] != 0) {
        ESP_LOGW(TAG, "Not a valid string");
        return -1;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *msg_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "cmd", "send");
    cJSON_AddStringToObject(msg_obj, "type", "customized");
    cJSON_AddStringToObject(msg_obj, "data", (char *)msg->data);
    char *msg_body = cJSON_PrintUnformatted(msg_obj);
    if (msg_body) {
        cJSON_AddStringToObject(json, "msg", msg_body);
    }
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    cJSON_Delete(msg_obj);
    int ret = 0;
    if (payload) {
        ESP_LOGI(TAG, "send to remote : %s", payload);
        ret = esp_websocket_client_send_text(wss->ws, payload, strlen(payload), portMAX_DELAY);
        free(payload);
    }
    free(msg_body);
    return ret > 0 ? 0 : -1;
}

static int wss_signal_send_msg(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg)
{
    // customized/BYE 走 WebSocket；SDP/candidate 按 AppRTC 约定 POST 到房间消息接口。
    wss_sig_t *sg = (wss_sig_t *)h;
    if (msg->type == ESP_PEER_SIGNALING_MSG_CUSTOMIZED) {
        return send_customized_data(sg->wss_client, msg);
    }
    char request_room[128];
    snprintf(request_room, 128, "%s/message/%s/%s",
             sg->client_info.base_url,
             sg->client_info.room_id, sg->client_info.client_id);
    ESP_LOGI(TAG, "Begin to send offer");
    if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        return send_bye(sg->wss_client, &sg->client_info);
    }
    cJSON *json = cJSON_CreateObject();
    if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        if (sg->ice_info.is_initiator) {
            cJSON_AddStringToObject(json, "type", "offer");
        } else {
            cJSON_AddStringToObject(json, "type", "answer");
        }
        cJSON_AddStringToObject(json, "sdp", (char *)msg->data);
    } else if (msg->type == ESP_PEER_SIGNALING_MSG_CANDIDATE) {
        cJSON_AddStringToObject(json, "type", "candidate");
        cJSON_AddStringToObject(json, "candidate", (char *)msg->data);
    } else {
        return 0;
    }
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    int ret = https_post(request_room, NULL, payload, NULL, NULL, NULL);
    free(payload);
    return ret;
}

static void wss_send_leave(wss_sig_t *sg)
{
    // 离开时同时通知房间 HTTP 服务和对端 WebSocket，最后删除 WebSocket 注册记录。
    char request_room[128];
    snprintf(request_room, 128, "%s/leave/%s/%s",
             sg->client_info.base_url,
             sg->client_info.room_id, sg->client_info.client_id);
    char header[128];
    char *headers[2] = { header, NULL };
    snprintf(header, 128, "Referer: %s/r/%s", sg->client_info.base_url, sg->client_info.room_id);
    https_post(request_room, headers, NULL, NULL, NULL, NULL);
    esp_peer_signaling_msg_t msg = {
        .type = ESP_PEER_SIGNALING_MSG_BYE,
    };
    wss_signal_send_msg(sg, &msg);
    media_lib_thread_sleep(100);
    snprintf(request_room, 128, "%s/%s/%s",
             sg->client_info.wss_post_url,
             sg->client_info.room_id, sg->client_info.client_id);
    https_send_request("DELETE", NULL, request_room, NULL, NULL, NULL, NULL);
}

int wss_signal_stop(esp_peer_signaling_handle_t h)
{
    // 先发送离开并销毁 WebSocket，再停止 ICE 刷新任务，最后释放两类响应数据。
    wss_sig_t *sg = (wss_sig_t *)h;
    if (sg->wss_client) {
        printf("Before to send leave....\n");
        wss_send_leave(sg);
        destroy_wss(sg->wss_client);
    }
    stop_reload_ice_timer(sg);
    free_client_info(&sg->client_info);
    free_ice_info(&sg->ice_info);
    free(sg);
    return 0;
}

const esp_peer_signaling_impl_t *esp_signaling_get_apprtc_impl(void)
{
    static const esp_peer_signaling_impl_t impl = {
        .start = wss_signal_start,
        .send_msg = wss_signal_send_msg,
        .stop = wss_signal_stop,
    };
    return &impl;
}
