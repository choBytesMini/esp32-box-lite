# ESP32-S3-BOX-Lite 与服务器接口文档

## 概述

ESP32-S3-BOX-Lite 设备作为智能家居交互终端，通过以下协议与服务器通信：

- **WebSocket**: 实时音频流上传（Opus编码）
- **MQTT**: 控制信令、状态管理、消息推送
- **HTTP**: TTS音频文件下载

## 1. WebSocket 接口

### 1.1 连接配置

```c
// config.h
#define WS_URL "ws://82.158.224.81:8888/ws"
#define WS_SEND_TIMEOUT_MS 5000
```

### 1.2 握手机制

设备连接后立即发送Hello消息：

```json
{
  "type": "hello",
  "version": 3,
  "transport": "websocket",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

### 1.3 二进制帧协议 (BinaryProtocol3)

每个音频帧格式：

```
Offset  Size  Type      Field
  0      1    uint8     type (0x00=Opus音频, 0x01=JSON)
  1      1    uint8     reserved (始终为0)
  2      2    uint16    payload_size (网络字节序big-endian)
  4      N    uint8[]   payload (Opus编码数据)
```

### 1.4 Opus编码参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 采样率 | 16000 Hz | |
| 通道 | 1 (mono) | |
| 位深 | 16-bit signed | |
| 帧时长 | 60 ms | 每帧960个PCM样本 |
| 码率 | AUTO (VBR) | 可变码率 |
| DTX | 开启 | 静音时不产生数据 |

### 1.5 会话结束信号

```json
{"type": "stt_end"}
```

## 2. MQTT 接口

### 2.1 连接配置

```c
// config.h
#define MQTT_HOST "82.158.224.81"
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID_PREFIX "esp32_living_room"
```

### 2.2 设备 → 服务器

| Topic | Payload | 时机 | QoS |
|-------|---------|------|-----|
| `home/status/esp32` | `{"status":"online"}` | 设备上线 | 1 (retain) |
| `home/status/esp32` | `{"status":"offline"}` | 设备断开 (Last Will) | 1 (retain) |

### 2.3 服务器 → 设备

| Topic | QoS | Payload字段 | 说明 |
|-------|-----|-------------|------|
| `home/agent/reply` | 1 | user, text, tts_url | 语音回复 |
| `home/agent/identity` | 1 | user, confidence | 人脸识别结果 |
| `home/alert` | 2 | (任意JSON) | 安全警报 |
| `home/music/status` | 1 | state, track, artist | 音乐播放状态 |
| `home/agent/skill` | 1 | user, skill | 技能切换通知 |

### 2.4 Payload示例

```json
// home/agent/reply
{"user": "小管家", "text": "今天天气晴, 25°C", "tts_url": "http://host/tts/001.wav"}

// home/agent/identity
{"user": "张三", "confidence": 0.95}

// home/alert
{"type": "smoke", "level": "high"}

// home/music/status
{"state": "playing", "track": "晴天", "artist": "周杰伦"}
// {"state": "paused"}
// {"state": "stopped"}
// {"state": "not_found"}

// home/agent/skill
{"user": "小管家", "skill": "weather"}
```

## 3. HTTP 接口

### 3.1 TTS音频下载

设备通过HTTP GET下载TTS音频文件：

```
GET {tts_url} HTTP/1.1
Host: {服务器地址}
```

### 3.2 支持格式

| 参数 | 支持格式 | 说明 |
|------|---------|------|
| 文件类型 | MP3 或 WAV | |
| WAV格式 | PCM int16, 44100Hz, 1-2ch | 44字节RIFF头自动跳过 |
| MP3格式 | 任意码率 | 通过esp_audio_codec解码 |
| 最大文件 | 500,000字节 | TTS_MAX_SIZE_BYTES |
| 下载超时 | 10秒 | TTS_DOWNLOAD_TIMEOUT_MS |

## 4. 完整会话流程

```
1. 设备启动
   → WiFi连接
   → MQTT连接 → publish {"status":"online"}
   → WebSocket连接 → send hello → 等待确认

2. 等待唤醒词 "小智小智"

3. 唤醒词检测到
   → 开始流式录音
   → WebSocket发送BinaryProtocol3(Opus)帧

4. 用户停止说话 (VAD静音3秒)
   → 发送 {"type":"stt_end"}
   → LCD显示 "管家思考中..."

5. 服务器处理
   → STT → NLP → TTS
   → MQTT publish home/agent/reply

6. 设备接收回复
   → LCD显示回复文本
   → HTTP GET下载tts_url
   → 扬声器播放TTS音频

7. 返回步骤2
```

## 5. 关键数值速查

| 项目 | 值 |
|------|-----|
| Opus采样率 | 16000 Hz |
| Opus帧长 | 60ms (960 samples) |
| 每帧Opus字节 | ~50-150字节 (VBR) |
| 上传协议 | WebSocket Binary (4B头 + Opus) |
| 上传URL | ws://82.158.224.81:8888/ws |
| TTS下载 | HTTP GET, MP3/WAV, 44100Hz |
| MQTT Broker | mqtt://82.158.224.81:1883 |
| MIC采样率 | 16000 Hz, 16bit |
| SPK采样率 | 44100 Hz, 16bit |

## 6. 错误处理

### 6.1 WebSocket断开重连

设备主循环每秒检测连接状态，断开时自动重连并重新发送Hello消息。

### 6.2 MQTT断开重连

MQTT客户端自动重连，重连后重新订阅所有主题并发布online状态。

### 6.3 WiFi断开重连

设备主循环检测WiFi状态，断开时自动重新连接。

## 7. 代码实现参考

### 7.1 WebSocket客户端

```c
// websocket_uploader.h
bool ws_uploader_connect(const char *url);
void ws_uploader_disconnect(void);
bool ws_uploader_is_connected(void);
bool ws_uploader_send_binary(const uint8_t *data, size_t len);
bool ws_uploader_send_text(const char *text);
void ws_uploader_set_on_text(ws_on_text_cb_t cb, void *user_ctx);
```

### 7.2 MQTT客户端

```c
// app_mqtt_client.h
bool mqtt_client_init(const char *host, int port);
void mqtt_client_on_reply(mqtt_reply_cb_t cb);
void mqtt_client_on_identity(mqtt_identity_cb_t cb);
void mqtt_client_on_alert(mqtt_alert_cb_t cb);
void mqtt_client_on_music_status(mqtt_music_status_cb_t cb);
void mqtt_client_on_skill(mqtt_skill_cb_t cb);
bool mqtt_client_publish_status(const char *status);
bool mqtt_client_publish(const char *topic, const char *payload);
```

### 7.3 HTTP客户端

```c
// http_uploader.h
bool http_uploader_upload(const char *host, int port, const char *path,
                          const uint8_t *data, size_t size, http_response_t *resp);
```

### 7.4 音频流协议

```c
// audio_stream_protocol.h
#define AUDIO_STREAM_FRAME_DURATION_MS  60
#define AUDIO_STREAM_SAMPLE_RATE        16000
#define AUDIO_STREAM_CHANNELS           1

enum {
    AUDIO_STREAM_TYPE_OPUS = 0x00,
    AUDIO_STREAM_TYPE_JSON = 0x01,
};

typedef struct {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t payload_size;
    uint8_t  payload[];
} __attribute__((packed)) binary_protocol_v3_t;
```

## 8. 配置参数

### 8.1 网络配置

```c
#define WIFI_SSID "chobits"
#define WIFI_PASSWORD "chobytes"
#define WIFI_TIMEOUT_MS 15000

#define SERVER_HOST "82.158.224.81"
#define MQTT_HOST "82.158.224.81"
#define SERVER_HTTP_PORT 5000
#define MQTT_PORT 1883

#define WS_URL "ws://82.158.224.81:8888/ws"
#define WS_SEND_TIMEOUT_MS 5000

#define HTTP_UPLOAD_TIMEOUT_MS 10000
```

### 8.2 音频配置

```c
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 44100
#define AUDIO_MAX_DURATION_MS    5000
#define AUDIO_BUFFER_SIZE        (AUDIO_INPUT_SAMPLE_RATE * 2 * AUDIO_MAX_DURATION_MS / 1000)
#define AUDIO_MIN_VALID_BYTES    3200

#define TTS_DOWNLOAD_TIMEOUT_MS  10000
#define TTS_MAX_SIZE_BYTES       500000
```