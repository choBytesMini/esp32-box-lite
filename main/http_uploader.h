/**
 * @file http_uploader.h
 * @brief HTTP 上传工具接口
 *
 * 上传音频数据到服务器（POST 请求）。
 */

#ifndef _HTTP_UPLOADER_H_
#define _HTTP_UPLOADER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTTP 响应结构 */
typedef struct {
    int  status_code;    // HTTP 状态码
    char body[512];      // 响应体
} http_response_t;

/**
 * @brief 上传数据到 HTTP 服务器
 *
 * @param host 服务器地址
 * @param port 端口
 * @param path 路径
 * @param data 数据指针
 * @param size 数据大小
 * @param resp 响应结果
 * @return true=成功，false=失败
 */
bool http_uploader_upload(const char *host, int port, const char *path,
                          const uint8_t *data, size_t size, http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif // _HTTP_UPLOADER_H_