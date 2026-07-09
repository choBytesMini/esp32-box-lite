# ESP32-S3-BOX-Lite 与服务器接口文档

## 概述

ESP32-S3-BOX-Lite 设备作为智能家居交互终端，通过以下协议与服务器通信：

- **WebSocket**: 实时音频流上传（Opus编码）+ 服务器音频推送播放（PCM）
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
  0      1    uint8     type (0x00=Opus音频, 0x01=PCM音频/JSON)
  1      1    uint8     reserved (始终为0)
  2      2    uint16    payload_size (网络字节序big-endian)
  4      N    uint8[]   payload
```

**帧类型说明：**

| type | 方向 | 说明 |
|------|------|------|
| 0x00 | 设备→服务器 | Opus编码音频帧 |
| 0x01 | 服务器→设备 | PCM原始音频帧（16kHz, 16bit, mono） |

### 1.4 Opus编码参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 采样率 | 16000 Hz | |
| 通道 | 1 (mono) | |
| 位深 | 16-bit signed | |
| 帧时长 | 60 ms | 每帧960个PCM样本 |
| 码率 | AUTO (VBR) | 可变码率 |
| DTX | 开启 | 静音时不产生数据 |

### 1.5 服务器音频推送

服务器通过 `/api/audio/send` HTTP API 推送音频到设备：

```
POST http://服务器:5000/api/audio/send
Content-Type: application/json

{"path": "/path/to/audio.mp3"}
```

服务器将MP3解码为PCM后，通过WebSocket发送BP3帧（type=0x01, 1920字节/帧）到设备。设备直接入队播放，不需要Opus解码。

### 1.6 会话结束信号

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
| `home/agent/identity` | 1 | user, confidence | 声纹识别结果 |
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
| WAV格式 | PCM int16, 16000Hz, 1ch | 44字节RIFF头自动跳过 |
| MP3格式 | 任意码率 | 通过esp_audio_codec解码 |
| 最大文件 | 500,000字节 | TTS_MAX_SIZE_BYTES |
| 下载超时 | 10秒 | TTS_DOWNLOAD_TIMEOUT_MS |

## 4. 完整会话流程

```
1. 设备启动
   → WiFi连接
   → MQTT连接 → publish {"status":"online"}
   → WebSocket连接 → send hello → 等待确认
   → 启动 stream_play_task（准备接收服务器音频）

2. 等待唤醒词 "你好小安"

3. 唤醒词检测到
   → audio_player_stop() 停止播放任务
   → 启动流式录音
   → WebSocket发送BinaryProtocol3(Opus)帧
   → 静默检测：RMS > 200 更新时间戳，15秒无语音停止

4. 用户停止说话 (15秒静默超时)
   → audio_recorder_stop_stream()
   → 发送 {"type":"stt_end"}

5. 服务器处理
   → STT → LLM → TTS
   → MQTT publish home/agent/reply
   → WebSocket推送PCM音频帧（type=0x01）

6. 设备接收回复
   → on_ws_binary 接收PCM帧 → 入队播放
   → stream_play_task → i2s_channel_write → 扬声器
   → LCD显示回复文本
   → HTTP GET下载tts_url（可选）

7. 返回步骤2
```

## 5. 关键数值速查

| 项目 | 值 |
|------|-----|
| 唤醒词 | "你好小安" (wn9_nihaoxiaoan_tts2) |
| Opus采样率 | 16000 Hz |
| Opus帧长 | 60ms (960 samples) |
| 每帧Opus字节 | ~50-150字节 (VBR) |
| 上传协议 | WebSocket Binary (4B头 + Opus) |
| 服务器推送协议 | WebSocket Binary (4B头 + PCM, 1920字节/帧) |
| 上传URL | ws://82.158.224.81:8888/ws |
| TTS下载 | HTTP GET, MP3/WAV, 16000Hz |
| MQTT Broker | mqtt://82.158.224.81:1883 |
| MIC采样率 | 16000 Hz, 16bit, stereo→mono(左声道) |
| SPK采样率 | 16000 Hz, 16bit, mono |
| 默认音量 | 100% |
| 软件增益 | 3x（仅用于Opus编码，RMS检测用原始数据） |
| 静默超时 | 15秒（RMS阈值200） |
| 流播放队列 | 64帧，PSRAM分配（120KB） |
| I2S TX DMA | 12描述符 × 480帧 |

## 6. 错误处理

### 6.1 WebSocket断开重连

设备主循环每秒检测连接状态，断开时自动重连并重新发送Hello消息。

### 6.2 MQTT断开重连

MQTT客户端自动重连，重连后重新订阅所有主题并发布online状态。

### 6.3 WiFi断开重连

设备主循环检测WiFi状态，断开时自动重新连接。

### 6.4 I2S TX通道状态

`esp_codec_dev_open(s_spk_handle)` 内部会 disable TX 再 reconfig，必须在之后调用 `i2s_channel_enable(tx_handle)` 显式重新启用。

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
void ws_uploader_set_on_binary(ws_on_binary_cb_t cb, void *user_ctx);
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

### 7.4 音频播放器

```c
// audio_player.h
void audio_player_set_codec(audio_codec_t *codec);
void audio_player_stream_start(void);      // 启动流播放任务（PSRAM队列）
void audio_player_stop(void);              // 停止播放并销毁任务
bool audio_player_stream_queue(const int16_t *data, size_t samples);  // 入队PCM数据
void audio_player_set_volume(int volume);  // 设置音量0-100
int audio_player_get_volume(void);
```

### 7.5 音频流协议

```c
// audio_stream_protocol.h
#define AUDIO_STREAM_FRAME_DURATION_MS  60
#define AUDIO_STREAM_SAMPLE_RATE        16000
#define AUDIO_STREAM_CHANNELS           1
#define AUDIO_STREAM_FRAME_SAMPLES      960

enum {
    AUDIO_STREAM_TYPE_OPUS = 0x00,
    AUDIO_STREAM_TYPE_PCM  = 0x01,  // 服务器推送PCM音频
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
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define AUDIO_MAX_DURATION_MS    5000
#define AUDIO_BUFFER_SIZE        (AUDIO_INPUT_SAMPLE_RATE * 2 * AUDIO_MAX_DURATION_MS / 1000)
#define AUDIO_MIN_VALID_BYTES    3200

#define TTS_DOWNLOAD_TIMEOUT_MS  10000
#define TTS_MAX_SIZE_BYTES       500000

#define VOLUME_DEFAULT           100     // 默认音量100%
#define SOFTWARE_GAIN            3       // 软件增益3x（仅用于编码）
#define VOICE_RMS_THRESHOLD      200     // RMS静默阈值
#define SILENCE_TIMEOUT_MS       15000   // 静默超时15秒

#define STREAM_QUEUE_DEPTH       64      // 流播放队列深度
```

### 8.3 I2S配置

```c
// I2S引脚（config.h）
#define I2S_MCLK_PIN   38
#define I2S_BCLK_PIN   14
#define I2S_WS_PIN     13
#define I2S_DIN_PIN    12
#define I2S_DOUT_PIN   11
#define PA_ENABLE_PIN  46      // 功放使能引脚

// I2S参数
// TX: STD Philips, 16kHz, 16bit, mono
// RX: STD Philips, 16kHz, 16bit, stereo
// DMA TX: 12描述符 × 480帧
// DMA RX: 6描述符 × 240帧
```

### 8.4 FreeRTOS任务配置

```c
#define TASK_RECORD_CORE         1       // 录音任务必须在Core 1（I2S DMA绑定）
#define TASK_RECORD_STACK_SIZE   32768
#define TASK_PLAY_CORE           0       // 播放任务在Core 0
#define TASK_PLAY_STACK_SIZE     16384
```

## 9. 源文件列表

| 文件 | 说明 |
|------|------|
| `main.c` | 主入口，唤醒词任务，WebSocket回调 |
| `config.h` | 全局配置宏 |
| `audio_codec.c/h` | I2S初始化，ES8156/ES7243E编解码器 |
| `audio_player.c/h` | 流播放任务，音量控制 |
| `audio_recorder.c/h` | 缓冲录音，流式录音 |
| `audio_stream_protocol.h` | BP3帧格式定义 |
| `opus_encoder.c/h` | Opus编码器 |
| `opus_decoder.c/h` | Opus解码器（原生libopus） |
| `websocket_uploader.c/h` | WebSocket客户端 |
| `app_mqtt_client.c/h` | MQTT客户端 |
| `wifi_manager.c/h` | WiFi连接管理 |
| `wake_word_detector.c/h` | 唤醒词检测（ESP-SR AFE+WakeNet） |
| `http_uploader.c/h` | HTTP客户端 |
| `lcd_display.c/h` | LCD显示（ILI9341） |