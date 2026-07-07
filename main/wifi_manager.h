/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理接口
 *
 * 提供 WiFi STA 模式连接和状态查询。
 */

#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 连接 WiFi
 *
 * @param ssid      WiFi 名称
 * @param password  WiFi 密码
 * @param timeout_ms 连接超时（毫秒）
 * @return true=连接成功，false=超时
 */
bool wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/** 检查 WiFi 是否已连接 */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // _WIFI_MANAGER_H_