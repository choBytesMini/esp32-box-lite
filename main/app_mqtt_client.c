/**
 * @file app_mqtt_client.c
 * @brief MQTT 客户端模块 — 订阅服务器 topic，分发回调
 *
 * 订阅的 topic：
 *   - home/agent/reply      → AI 回复（文本 + TTS URL）
 *   - home/agent/identity   → 声纹识别结果
 *   - home/alert            → 告警通知
 *   - home/music/status     → 音乐播放状态
 *   - home/agent/skill      → 技能调用
 *
 * 使用 ESP-IDF 的 mqtt_client 组件，支持遗嘱消息检测离线。
 */

#include "app_mqtt_client.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "config.h"
#include <string.h>

static const char *TAG = "MqttClient";
static esp_mqtt_client_handle_t s_client = NULL;

/** 回调函数指针 */
static mqtt_reply_cb_t        s_reply_cb = NULL;
static mqtt_identity_cb_t     s_identity_cb = NULL;
static mqtt_alert_cb_t        s_alert_cb = NULL;
static mqtt_music_status_cb_t s_music_cb = NULL;
static mqtt_skill_cb_t        s_skill_cb = NULL;

/** 订阅所有 topic */
static void subscribe_all(void) {
    esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_REPLY, 1);
    esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_IDENTITY, 1);
    esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_ALERT, 2);
    esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_MUSIC_STATUS, 1);
    esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_SKILL, 1);
}

/**
 * @brief 解析 MQTT 消息并分发到对应回调
 *
 * 根据 topic 中的关键词匹配：
 *   - "agent/reply" → 回复回调
 *   - "agent/identity" → 声纹回调
 *   - "alert" → 告警回调
 *   - "music/status" → 音乐状态回调
 *   - "agent/skill" → 技能回调
 */
static void handle_message(const char *topic, const char *data, int data_len) {
    char msg[MQTT_BUFFER_SIZE] = {};
    int len = (data_len < (int)sizeof(msg) - 1) ? data_len : (int)sizeof(msg) - 1;
    memcpy(msg, data, len);

    cJSON *json = cJSON_Parse(msg);
    if (!json) return;

    if (strstr(topic, "agent/reply") && s_reply_cb) {
        cJSON *user = cJSON_GetObjectItem(json, "user");
        cJSON *text = cJSON_GetObjectItem(json, "text");
 cJSON *tts = cJSON_GetObjectItem(json, "tts_url");
        s_reply_cb(cJSON_IsString(user) ? user->valuestring : "",
                   cJSON_IsString(text) ? text->valuestring : "",
                   cJSON_IsString(tts) ? tts->valuestring : "");
    } else if (strstr(topic, "agent/identity") && s_identity_cb) {
        cJSON *user = cJSON_GetObjectItem(json, "user");
        cJSON *conf = cJSON_GetObjectItem(json, "confidence");
        s_identity_cb(cJSON_IsString(user) ? user->valuestring : "",
                      cJSON_IsNumber(conf) ? (float)conf->valuedouble : 0.0f);
    } else if (strstr(topic, "alert") && s_alert_cb) {
        s_alert_cb();
    } else if (strstr(topic, "music/status") && s_music_cb) {
        cJSON *state = cJSON_GetObjectItem(json, "state");
        cJSON *track = cJSON_GetObjectItem(json, "track");
        cJSON *artist = cJSON_GetObjectItem(json, "artist");
        s_music_cb(cJSON_IsString(state) ? state->valuestring : "",
                   cJSON_IsString(track) ? track->valuestring : "",
                   cJSON_IsString(artist) ? artist->valuestring : "");
    } else if (strstr(topic, "agent/skill") && s_skill_cb) {
        cJSON *user = cJSON_GetObjectItem(json, "user");
        cJSON *skill = cJSON_GetObjectItem(json, "skill");
        s_skill_cb(cJSON_IsString(user) ? user->valuestring : "",
                   cJSON_IsString(skill) ? skill->valuestring : "");
    }

    cJSON_Delete(json);
}

/** MQTT 事件处理：连接→订阅，消息→分发，断开→日志 */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT 已连接");
            subscribe_all();
            mqtt_client_publish_status("online");
            break;
        case MQTT_EVENT_DATA:
            handle_message(event->topic, event->data, event->data_len);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT 断开连接");
            break;
        default:
            break;
    }
}

/**
 * @brief 初始化 MQTT 客户端
 *
 * 配置遗嘱消息（离线检测）和自动重连。
 */
bool mqtt_client_init(const char *host, int port) {
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", host, port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .session.last_will = {
            .topic = MQTT_TOPIC_STATUS,
            .msg = "{\"status\":\"offline\"}",
            .qos = 1,
            .retain = true,
        },
        .network.reconnect_timeout_ms = MQTT_RETRY_DELAY_MS,
        .buffer.size = MQTT_BUFFER_SIZE,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) return false;

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client) == ESP_OK;
}

/** 设置回调函数 */
void mqtt_client_on_reply(mqtt_reply_cb_t cb)              { s_reply_cb = cb; }
void mqtt_client_on_identity(mqtt_identity_cb_t cb)        { s_identity_cb = cb; }
void mqtt_client_on_alert(mqtt_alert_cb_t cb)              { s_alert_cb = cb; }
void mqtt_client_on_music_status(mqtt_music_status_cb_t cb) { s_music_cb = cb; }
void mqtt_client_on_skill(mqtt_skill_cb_t cb)              { s_skill_cb = cb; }

/** 发布设备状态（retained 消息） */
bool mqtt_client_publish_status(const char *status) {
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"status\":\"%s\"}", status);
    return esp_mqtt_client_publish(s_client, MQTT_TOPIC_STATUS, payload, 0, 1, true) >= 0;
}

/** 发布 MQTT 消息 */
bool mqtt_client_publish(const char *topic, const char *payload) {
    return esp_mqtt_client_publish(s_client, topic, payload, 0, 1, false) >= 0;
}

/** 检查 MQTT 客户端是否已初始化 */
bool mqtt_client_connected(void) {
    return s_client != NULL;
}