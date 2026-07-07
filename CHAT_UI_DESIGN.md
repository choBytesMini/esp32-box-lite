# ESP32-S3-BOX-Lite 聊天终端界面设计

## 屏幕布局 (320x240, ILI9341, RGB565)

```
┌──────────────────────────────────────────────┐  Y=0
│  Smart Home Assistant          WiFi  MQTT    │  头部 (28px)
│  ──────────────────────────────────────────  │  Y=27
│                                              │
│              Welcome to Smart Home!          │  聊天区域
│            Say 'Xiao Zhi' to start           │  (188px)
│                                              │
│  ┌──────────────────────┐                    │
│  │ System ready. Say     │  ← 系统消息 (居中)│
│  │ wake word to start.   │    深灰色气泡     │
│  └──────────────────────┘                    │
│                                              │
│                      ┌─────────────────────┐ │
│                      │ [User]: What's the  │ │  ← 用户消息 (右对齐)
│                      │ weather today?      │ │    深蓝色气泡
│                      └─────────────────────┘ │
│                                              │
│  ┌──────────────────────┐                    │
│  │ [AI]: Today is sunny  │  ← AI 回复 (左对齐)│
│  │ with a high of 25°C.  │    深绿色气泡     │
│  └──────────────────────┘                    │
│                                              │
│  ┌──────────────────────┐                    │
│  │ ALERT: Smoke Alert!   │  ← 错误消息 (居中)│
│  └──────────────────────┘                    │  深红色气泡
│                                              │
│  ──────────────────────────────────────────  │  Y=216
│  [LISTEN]  Listening...                      │  底部状态栏 (24px)
└──────────────────────────────────────────────┘  Y=239
```

## 颜色方案

| 元素 | 颜色 | RGB565 |
|------|------|--------|
| 背景 | 深灰 | `0x10A2` |
| 头部背景 | 深蓝灰 | `0x2124` |
| 状态栏背景 | 深青灰 | `0x18E3` |
| 用户气泡 | 深蓝 | `0x2D5B` |
| AI 气泡 | 深绿 | `0x2C8B` |
| 系统消息 | 深灰 | `0x4208` |
| 错误消息 | 深红 | `0x4000` |
| 强调色 (分隔线等) | 蓝绿 | `0x0C7F` |
| WiFi/MQTT 已连接 | 绿色 | `0x07E0` |
| WiFi/MQTT 断开 | 红色 | `0xF800` |

## 状态指示器

```
[READY ]  - 空闲等待唤醒词
[LISTEN]  - 正在录音
[THINK ]  - 服务端思考中
[SPEAK ]  - TTS 语音播报中
[ERROR ]  - 错误状态
```

## 新增 API

```c
// 聊天消息管理
lcd_chat_add_message(&lcd, MSG_TYPE_USER, "Hello");
lcd_chat_add_message(&lcd, MSG_TYPE_AI, "Hi there!");
lcd_chat_add_message(&lcd, MSG_TYPE_SYSTEM, "WiFi connected");
lcd_chat_add_message(&lcd, MSG_TYPE_ERROR, "Connection failed");

// 状态管理
lcd_chat_set_state(&lcd, CHAT_STATE_LISTENING, "Listening...");
lcd_set_wifi_status(&lcd, true);
lcd_set_mqtt_status(&lcd, true);

// 滚动控制
lcd_chat_scroll_up(&lcd);
lcd_chat_scroll_down(&lcd);
lcd_chat_scroll_to_bottom(&lcd);

// 界面刷新
lcd_refresh(&lcd);
lcd_chat_clear(&lcd);
```

## 消息历史

- 环形缓冲区存储最近 20 条消息
- 新消息自动滚动到底部
- 支持手动上下滚动查看历史
- 系统消息居中显示 (深灰色气泡)
- 用户消息右对齐 (深蓝色气泡)
- AI 回复左对齐 (深绿色气泡)
- 错误消息居中显示 (深红色气泡)

## 帧缓冲优化

- 使用 PSRAM 帧缓冲 (320×240×2 = 150KB)
- 先绘制到缓冲区，再一次性刷新到 LCD
- 减少屏幕闪烁
- 支持部分区域刷新 (未来优化)
