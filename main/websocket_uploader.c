/**
 * @file websocket_uploader.c
 * @brief WebSocket 通信模块
 *
 * 使用 esp_transport 实现 WebSocket 客户端：
 *   - 连接/断开
 *   - 发送二进制帧（Opus 音频）和文本帧（JSON 命令）
 *   - 接收任务分发文本/二进制帧到回调
 *   - 断开时安全停止接收任务
 */

#include "websocket_uploader.h"
#include "config.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WebSocket";

static esp_transport_handle_t s_tcp = NULL;
static esp_transport_handle_t s_ws = NULL;
static volatile bool s_recv_running = false;
static ws_on_text_cb_t s_on_text_cb = NULL;
static void *s_on_text_ctx = NULL;
static ws_on_binary_cb_t s_on_binary_cb = NULL;
static void *s_on_binary_ctx = NULL;

static char *ws_parse_url(const char *url, char **host_out, int *port_out, char **path_out) {
    char *url_copy = strdup(url);
    char *p = url_copy;

    if (strncmp(p, "ws://", 5) == 0) {
        *port_out = 80;
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        *port_out = 443;
        p += 6;
    } else {
        *port_out = 80;
    }

    *host_out = p;

    char *slash = strchr(p, '/');
    char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        *colon = '\0';
        *port_out = atoi(colon + 1);
        if (slash) {
            *path_out = slash;
        } else {
            *path_out = "/";
        }
    } else if (slash) {
        *slash = '\0';
        *path_out = slash;
    } else {
        *path_out = "/";
    }

    return url_copy;
}

static void ws_recv_task(void *arg) {
    char buf[2048];
    s_recv_running = true;

    while (s_ws && s_recv_running) {
        int len = esp_transport_read(s_ws, buf, sizeof(buf) - 1, 500);
        if (len > 0) {
            ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(s_ws);
            if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
                buf[len] = '\0';
                if (s_on_text_cb) {
                    s_on_text_cb(buf, len, s_on_text_ctx);
                }
            } else if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
                if (s_on_binary_cb) {
                    s_on_binary_cb((const uint8_t *)buf, len, s_on_binary_ctx);
                }
            } else if (opcode == WS_TRANSPORT_OPCODES_PING) {
                esp_transport_ws_send_raw(s_ws, WS_TRANSPORT_OPCODES_PONG, buf, len, 5000);
            }
        } else if (len < 0) {
            ESP_LOGW(TAG, "Read error, disconnecting");
            break;
        }
    }
    s_recv_running = false;
    vTaskDelete(NULL);
}

bool ws_uploader_connect(const char *url) {
    ws_uploader_disconnect();

    char *host = NULL;
    int port = 80;
    char *path = NULL;
    char *url_buf = ws_parse_url(url, &host, &port, &path);

    ESP_LOGI(TAG, "Connecting to ws://%s:%d%s", host, port, path);

    s_tcp = esp_transport_tcp_init();
    if (!s_tcp) {
        ESP_LOGE(TAG, "Failed to create TCP transport");
        free(url_buf);
        return false;
    }

    s_ws = esp_transport_ws_init(s_tcp);
    if (!s_ws) {
        ESP_LOGE(TAG, "Failed to create WS transport");
        esp_transport_destroy(s_tcp);
        s_tcp = NULL;
        free(url_buf);
        return false;
    }

    esp_transport_ws_config_t ws_cfg = {
        .ws_path = path,
    };
    esp_transport_ws_set_config(s_ws, &ws_cfg);

    int ret = esp_transport_connect(s_ws, host, port, 10000);
    free(url_buf);

    if (ret < 0) {
        ESP_LOGE(TAG, "Connection failed: %d", ret);
        esp_transport_destroy(s_ws);
        esp_transport_destroy(s_tcp);
        s_ws = NULL;
        s_tcp = NULL;
        return false;
    }

    ESP_LOGI(TAG, "WebSocket connected");
    xTaskCreate(ws_recv_task, "ws_recv", 8192, NULL, 3, NULL);
    return true;
}

void ws_uploader_disconnect(void) {
    // 先通知 recv task 退出
    s_recv_running = false;
    if (s_ws) {
        esp_transport_close(s_ws);
        // 等待 recv task 退出
        for (int i = 0; i < 20 && s_recv_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        esp_transport_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_tcp) {
        esp_transport_destroy(s_tcp);
        s_tcp = NULL;
    }
}

bool ws_uploader_is_connected(void) {
    return s_ws != NULL;
}

bool ws_uploader_send_binary(const uint8_t *data, size_t len) {
    if (!s_ws) return false;
    int ret = esp_transport_write(s_ws, (const char *)data, (int)len, WS_SEND_TIMEOUT_MS);
    return ret == (int)len;
}

bool ws_uploader_send_text(const char *text) {
    if (!s_ws) return false;
    int len = (int)strlen(text);
    int ret = esp_transport_ws_send_raw(s_ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                                         text, len, WS_SEND_TIMEOUT_MS);
    return ret == len;
}

void ws_uploader_set_on_text(ws_on_text_cb_t cb, void *user_ctx) {
    s_on_text_cb = cb;
    s_on_text_ctx = user_ctx;
}

void ws_uploader_set_on_binary(ws_on_binary_cb_t cb, void *user_ctx) {
    s_on_binary_cb = cb;
    s_on_binary_ctx = user_ctx;
}
