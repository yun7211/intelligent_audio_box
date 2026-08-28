/* 小智 WebRTC 本地 HTTPS 信令
 *
 * 移植自 esp-webrtc-solution 的 doorbell_local/webrtc_http_server.c，并删除门铃专用
 * 提示音端点 /webrtc/ring.aac。esp_signaling_get_http_impl() 在设备上提供：
 *   - GET  /webrtc/signal        ：SSE 流（设备 -> 浏览器）
 *   - POST /webrtc/signal/post   ：offer/answer/candidate/bye/customized（浏览器 -> 设备）
 *   - GET  /webrtc/test          ：固件内置的通话网页
 *
 * 依赖通过 main/CMakeLists.txt 的 EMBED_TXTFILES 嵌入：webrtc_test.html、
 * certs/servercert.pem 和 certs/prvtkey.pem。
 *
 * HTTPS 是必需的：浏览器只会在安全源中授权 getUserMedia（麦克风/摄像头）。自签名证书会
 * 在首次访问时触发一次安全警告。
 *
 * **这两个端点没有鉴权**：同一局域网里的任何主机都能 POST 一条信令消息，把自己变成对端。
 * 门铃模式下这一点很要紧 —— 开门命令只认 DataChannel（见 DoorbellController），而 DataChannel
 * 挡得住"往信令里注入消息"，挡不住"完成握手当上对端"。也就是说这条路径下，门锁前面的实际
 * 门槛就是"在同一个 WiFi 里"。补鉴权要把认证、重放防护、凭证轮换全做对，本文件一件都没做；
 * 需要更高门槛就切到 WEBRTC_SIGNALING_APPRTC_WS（那条路至少还要先知道房间号）。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_peer_signaling.h"
#include "webrtc_http_server.h"
#include "media_lib_socket.h"

#define MAX_CONTENT_LEN          (16 * 1024)
#define MAX_SIGNALING_QUEUE_SIZE 10

static const char *TAG = "WEBRTC_HTTP";

static httpd_handle_t server = NULL;
static QueueHandle_t signaling_queue = NULL;
static esp_peer_signaling_cfg_t sig_cfg = { 0 };
static bool event_stream_connected = false;
static bool event_stream_stopping  = false;
static httpd_req_t *event_stream_req = NULL;

static int send_event_stream_msg(httpd_req_t *req, char *data)
{
    int len = strlen(data) + strlen("data: ") + strlen("\n\n") + 1;
    char *buf = malloc(len);
    if (buf == NULL) {
        return -1;
    }
    len = snprintf(buf, len, "data: %s\n\n", data);
    int ret = httpd_resp_send_chunk(req, buf, len);
    free(buf);
    return ret;
}

static void signaling_msg_send_task(void *arg)
{
    uint32_t hear_beat = esp_timer_get_time() / 1000;
    while (!event_stream_stopping) {
        char *msg = NULL;
        int ret = 0;
        if (xQueueReceive(signaling_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (msg) {
                ret = send_event_stream_msg(event_stream_req, msg);
                free(msg);
                if (ret != 0) {
                    break;
                }
            }
        }
        // 心跳让浏览器及时发现设备离线，也避免中间设备回收空闲 SSE 连接。
        if (esp_timer_get_time() / 1000 - hear_beat > 5000) {
            hear_beat = esp_timer_get_time() / 1000;
            ret = send_event_stream_msg(event_stream_req, "{\"type\":\"heartbeat\"}");
            if (ret != 0) {
                ESP_LOGE(TAG, "Failed to send heartbeat ret %d", ret);
                break;
            }
        }
    }
    httpd_req_async_handler_complete(event_stream_req);
    event_stream_req = NULL;
    event_stream_connected = false;
    event_stream_stopping = false;
    ESP_LOGI(TAG, "Event Stream hdlr Quit");
    vTaskDelete(NULL);
}

// GET /webrtc/signal —— SSE（设备 -> 浏览器）。
static esp_err_t webrtc_signal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (event_stream_connected) {
        send_event_stream_msg(req, "{\"error\":\"Only allow one listener\"}");
        return 0;
    }
    send_event_stream_msg(req, "{\"type\":\"connected\"}");
    event_stream_connected = true;
    httpd_req_async_handler_begin(req, &event_stream_req);
    if (event_stream_req) {
        xTaskCreate(signaling_msg_send_task, "signal_hdlr", 4096, NULL, 5, NULL);
    }
    return ESP_OK;
}

// POST /webrtc/signal/post —— 信令消息（浏览器 -> 设备）。
static esp_err_t webrtc_signal_post_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (req->content_len > MAX_CONTENT_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_OK;
    }
    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No memory");
        return ESP_OK;
    }

    int ret = ESP_OK;
    cJSON *root = NULL;

    int readed = 0;
    while (readed < req->content_len) {
        ret = httpd_req_recv(req, buf + readed, req->content_len - readed);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ret = ESP_FAIL;
            goto _exit;
        }
        readed += ret;
    }
    buf[readed] = '\0';
    ret = ESP_OK;

    root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        ret = ESP_FAIL;
        goto _exit;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !type->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing type");
        ret = ESP_FAIL;
        goto _exit;
    }

    esp_peer_signaling_msg_t msg = { 0 };

    if (strcmp(type->valuestring, "offer") == 0) {
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (!sdp || !sdp->valuestring) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SDP");
            ret = ESP_FAIL;
            goto _exit;
        }
        msg.type = ESP_PEER_SIGNALING_MSG_SDP;
        msg.data = (uint8_t *)sdp->valuestring;
        msg.size = strlen(sdp->valuestring);
    } else if (strcmp(type->valuestring, "answer") == 0) {
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        if (!sdp || !sdp->valuestring) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SDP");
            ret = ESP_FAIL;
            goto _exit;
        }
        msg.type = ESP_PEER_SIGNALING_MSG_SDP;
        msg.data = (uint8_t *)sdp->valuestring;
        msg.size = strlen(sdp->valuestring);

        // 从 SDP 中拆出 ICE candidate 单独转发，适配 esp_peer 的信令回调格式。
        char *sdp_str = strdup(sdp->valuestring);
        if (sdp_str) {
            char *line = strtok(sdp_str, "\r\n");
            while (line != NULL) {
                if (strncmp(line, "a=candidate:", 11) == 0) {
                    esp_peer_signaling_msg_t candidate_msg = { 0 };
                    candidate_msg.type = ESP_PEER_SIGNALING_MSG_CANDIDATE;
                    candidate_msg.data = (uint8_t *)line;
                    candidate_msg.size = strlen(line);
                    sig_cfg.on_msg(&candidate_msg, sig_cfg.ctx);
                }
                line = strtok(NULL, "\r\n");
            }
            free(sdp_str);
        }
    } else if (strcmp(type->valuestring, "candidate") == 0) {
        cJSON *candidate = cJSON_GetObjectItem(root, "candidate");
        if (!candidate || !candidate->valuestring) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing candidate string");
            ret = ESP_FAIL;
            goto _exit;
        }
        msg.type = ESP_PEER_SIGNALING_MSG_CANDIDATE;
        msg.data = (uint8_t *)candidate->valuestring;
        msg.size = strlen(candidate->valuestring);
    } else if (strcmp(type->valuestring, "bye") == 0) {
        msg.type = ESP_PEER_SIGNALING_MSG_BYE;
    } else if (strcmp(type->valuestring, "customized") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!data || !data->valuestring) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing data for customized message");
            ret = ESP_FAIL;
            goto _exit;
        }
        msg.type = ESP_PEER_SIGNALING_MSG_CUSTOMIZED;
        msg.data = (uint8_t *)data->valuestring;
        msg.size = strlen(data->valuestring);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown message type");
        ret = ESP_FAIL;
        goto _exit;
    }
    if (msg.type) {
        sig_cfg.on_msg(&msg, sig_cfg.ctx);
    }
    httpd_resp_sendstr(req, "OK");
_exit:
    free(buf);
    cJSON_Delete(root);
    return ret;
}

// GET /webrtc/test —— 返回固件内置的通话测试网页。
static esp_err_t webrtc_test_get_handler(httpd_req_t *req)
{
    extern const unsigned char webrtc_test_html_start[] asm("_binary_webrtc_test_html_start");
    extern const unsigned char webrtc_test_html_end[]   asm("_binary_webrtc_test_html_end");
    const size_t webrtc_test_html_size = (webrtc_test_html_end - webrtc_test_html_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)webrtc_test_html_start, webrtc_test_html_size);
    return ESP_OK;
}

static esp_err_t init_http_server(void)
{
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_uri_handlers = 16;
    conf.httpd.stack_size = 8192;
    conf.httpd.max_open_sockets = 7;   // 网页、SSE 长连接和浏览器探测连接
    conf.httpd.lru_purge_enable = true;
    conf.httpd.recv_wait_timeout = 5;
    conf.httpd.send_wait_timeout = 5;
    conf.httpd.max_resp_headers = 8;

    // 浏览器的推测性 TLS 探测经常主动中断，这是预期行为，降低对应错误日志噪声。
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
    esp_log_level_set("esp_https_server", ESP_LOG_NONE);
    esp_log_level_set("httpd", ESP_LOG_NONE);

    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "Error starting server!");
        return -1;
    }

    httpd_uri_t webrtc_signal_get = {
        .uri = "/webrtc/signal", .method = HTTP_GET,
        .handler = webrtc_signal_get_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &webrtc_signal_get);

    httpd_uri_t webrtc_signal_post = {
        .uri = "/webrtc/signal/post", .method = HTTP_POST,
        .handler = webrtc_signal_post_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &webrtc_signal_post);

    httpd_uri_t webrtc_test = {
        .uri = "/webrtc/test", .method = HTTP_GET,
        .handler = webrtc_test_get_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &webrtc_test);

    return ESP_OK;
}

static esp_err_t webrtc_http_server_init(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h)
{
    signaling_queue = xQueueCreate(MAX_SIGNALING_QUEUE_SIZE, sizeof(char *));
    if (!signaling_queue) {
        ESP_LOGE(TAG, "Failed to create signaling queue");
        return ESP_FAIL;
    }
    esp_err_t ret = init_http_server();
    if (ret != ESP_OK) {
        vQueueDelete(signaling_queue);
        signaling_queue = NULL;
        return ret;
    }
    sig_cfg = *cfg;
    esp_peer_signaling_ice_info_t ice_info = {
        .is_initiator = true,
        .server_info.stun_url = "stun:stun.l.google.com:19302",
    };
    sig_cfg.on_ice_info(&ice_info, cfg->ctx);
    sig_cfg.on_connected(cfg->ctx);
    *h = (esp_peer_signaling_handle_t)signaling_queue;
    ESP_LOGI(TAG, "Success to init http server");
    return ESP_OK;
}

static int webrtc_http_server_send_msg(esp_peer_signaling_handle_t sig, esp_peer_signaling_msg_t *msg);

static int resend_all_candidate(esp_peer_signaling_handle_t sig, char *str)
{
    char *sdp_str = strdup(str);
    if (sdp_str) {
        char *line = strtok(sdp_str, "\r\n");
        while (line != NULL) {
            if (strncmp(line, "a=candidate:", 11) == 0) {
                char *cand = line + 2;
                esp_peer_signaling_msg_t candidate_msg = { 0 };
                candidate_msg.type = ESP_PEER_SIGNALING_MSG_CANDIDATE;
                candidate_msg.data = (uint8_t *)cand;
                candidate_msg.size = strlen(cand);
                webrtc_http_server_send_msg(sig, &candidate_msg);
            }
            line = strtok(NULL, "\r\n");
        }
        free(sdp_str);
    }
    return 0;
}

static int webrtc_http_server_send_msg(esp_peer_signaling_handle_t sig, esp_peer_signaling_msg_t *msg)
{
    if (signaling_queue == NULL || sig != signaling_queue || msg == NULL) {
        return -1;
    }
    cJSON *msg_root = cJSON_CreateObject();
    if (msg_root == NULL) {
        return -1;
    }
    switch (msg->type) {
        case ESP_PEER_SIGNALING_MSG_SDP:
            cJSON_AddStringToObject(msg_root, "type", "offer");
            cJSON_AddStringToObject(msg_root, "sdp", (char *)msg->data);
            break;
        case ESP_PEER_SIGNALING_MSG_CANDIDATE:
            cJSON_AddStringToObject(msg_root, "type", "candidate");
            cJSON_AddStringToObject(msg_root, "candidate", (char *)msg->data);
            break;
        case ESP_PEER_SIGNALING_MSG_BYE:
            cJSON_AddStringToObject(msg_root, "type", "bye");
            break;
        case ESP_PEER_SIGNALING_MSG_CUSTOMIZED:
            cJSON_AddStringToObject(msg_root, "type", "customized");
            cJSON_AddStringToObject(msg_root, "data", (char *)msg->data);
            break;
        default:
            cJSON_Delete(msg_root);
            return 0;
    }

    char *json_str = cJSON_PrintUnformatted(msg_root);
    cJSON_Delete(msg_root);
    if (json_str) {
        if (xQueueSend(signaling_queue, &json_str, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to send message to signaling queue");
            free(json_str);
        }
    }
    if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        resend_all_candidate(sig, (char *)msg->data);
    }
    return 0;
}

static int webrtc_http_server_deinit(esp_peer_signaling_handle_t sig)
{
    if (signaling_queue == NULL || sig != signaling_queue) {
        return -1;
    }
    ESP_LOGI(TAG, "Start to stop https server");
    if (signaling_queue) {
        char *msg = NULL;
        while (xQueueReceive(signaling_queue, &msg, 0) == pdTRUE) {
            if (msg) {
                free(msg);
            }
        }
    }
    if (event_stream_connected) {
        event_stream_stopping = true;
        while (event_stream_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (server) {
        httpd_ssl_stop(server);
        server = NULL;
    }
    if (signaling_queue) {
        vQueueDelete(signaling_queue);
        signaling_queue = NULL;
    }
    ESP_LOGI(TAG, "End to stop https server");
    return 0;
}

const esp_peer_signaling_impl_t *esp_signaling_get_http_impl(void)
{
    static const esp_peer_signaling_impl_t impl = {
        .start = webrtc_http_server_init,
        .send_msg = webrtc_http_server_send_msg,
        .stop = webrtc_http_server_deinit,
    };
    return &impl;
}
