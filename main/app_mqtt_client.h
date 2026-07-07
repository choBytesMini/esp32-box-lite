/**
 * @file app_mqtt_client.h
 * @brief MQTT 客户端接口
 *
 * 订阅服务器 topic，分发回调：
 *   - AI 回复、声纹识别、告警、音乐状态、技能调用
 */

#ifndef _APP_MQTT_CLIENT_H_
#define _APP_MQTT_CLIENT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** AI 回复回调 */
typedef void (*mqtt_reply_cb_t)(const char *user, const char *text, const char *tts_url);

/** 声纹识别回调 */
typedef void (*mqtt_identity_cb_t)(const char *user, float confidence);

/** 告警回调 */
typedef void (*mqtt_alert_cb_t)(void);

/** 音乐状态回调 */
typedef void (*mqtt_music_status_cb_t)(const char *state, const char *track, const char *artist);

/** 技能调用回调 */
typedef void (*mqtt_skill_cb_t)(const char *user, const char *skill_name);

/** 初始化 MQTT 客户端 */
bool mqtt_client_init(const char *host, int port);

/** 设置回调函数 */
void mqtt_client_on_reply(mqtt_reply_cb_t cb);
void mqtt_client_on_identity(mqtt_identity_cb_t cb);
void mqtt_client_on_alert(mqtt_alert_cb_t cb);
void mqtt_client_on_music_status(mqtt_music_status_cb_t cb);
void mqtt_client_on_skill(mqtt_skill_cb_t cb);

/** 发布设备状态 */
bool mqtt_client_publish_status(const char *status);

/** 发布 MQTT 消息 */
bool mqtt_client_publish(const char *topic, const char *payload);

/** 检查 MQTT 是否已初始化 */
bool mqtt_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif // _APP_MQTT_CLIENT_H_