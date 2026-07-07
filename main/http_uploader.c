/**
 * @file http_uploader.c
 * @brief HTTP 上传模块 — POST 数据到服务器
 *
 * 使用 ESP-IDF 的 esp_http_client 组件，发送 POST 请求并获取响应。
 * 用于上传音频文件到服务器进行语音识别。
 */

#include "http_uploader.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "HttpUpload";

/**
 * @brief 上传数据到 HTTP 服务器
 *
 * @param host  服务器地址
 * @param port  服务器端口
 * @param path  URL 路径
 * @param data  要上传的数据
 * @param size  数据大小
 * @param resp  响应结构体（输出）
 * @return true=上传成功，false=失败
 */
bool http_uploader_upload(const char *host, int port, const char *path,
                          const uint8_t *data, size_t size, http_response_t *resp) {
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_UPLOAD_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    // 设置 POST 请求
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)data, size);

    // 执行请求
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp->status_code = esp_http_client_get_status_code(client);
        int read_len = esp_http_client_read_response(client, resp->body,
                                                      sizeof(resp->body) - 1);
        if (read_len >= 0) resp->body[read_len] = '\0';
        else resp->body[0] = '\0';
    } else {
        resp->status_code = -1;
        resp->body[0] = '\0';
        ESP_LOGE(TAG, "HTTP 请求失败: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return (err == ESP_OK && resp->status_code > 0);
}