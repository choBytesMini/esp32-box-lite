/**
 * @file websocket_uploader.h
 * @brief WebSocket 通信接口
 *
 * 管理与服务器的 WebSocket 连接：
 *   - 连接/断开
 *   - 发送二进制帧（Opus 音频）
 *   - 发送文本帧（JSON 命令）
 *   - 接收文本/二进制帧回调
 */

#ifndef _WEBSOCKET_UPLOADER_H_
#define _WEBSOCKET_UPLOADER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 文本帧接收回调 */
typedef void (*ws_on_text_cb_t)(const char *text, int len, void *user_ctx);

/** 二进制帧接收回调 */
typedef void (*ws_on_binary_cb_t)(const uint8_t *data, int len, void *user_ctx);

/** 连接 WebSocket 服务器 */
bool ws_uploader_connect(const char *url);

/** 断开 WebSocket 连接 */
void ws_uploader_disconnect(void);

/** 检查是否已连接 */
bool ws_uploader_is_connected(void);

/** 发送二进制帧（Opus 音频） */
bool ws_uploader_send_binary(const uint8_t *data, size_t len);

/** 发送文本帧（JSON 命令） */
bool ws_uploader_send_text(const char *text);

/** 设置文本帧接收回调 */
void ws_uploader_set_on_text(ws_on_text_cb_t cb, void *user_ctx);

/** 设置二进制帧接收回调 */
void ws_uploader_set_on_binary(ws_on_binary_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif