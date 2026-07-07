# ESP32-S3-BOX-Lite 聊天终端界面开发记录

## 问题总结

### 问题1：屏幕闪烁

**现象**：LCD屏幕持续闪烁，刷新频率异常高（每秒约270次）

**根本原因**：唤醒词模型加载失败后，`wake_word_wait()` 立即返回 `false`，形成死循环

```
wake_word_task 循环:
  lcd_chat_set_state(...)  ← 设置脏标记
  wake_word_wait()         ← 模型加载失败，立即返回false
  continue                 ← 无延迟，直接进入下一次循环
```

**串口日志证据**：
```
E (4951) MODEL_LOADER: Can not find model in partition table
E (4951) WakeWord: 加载语音模型失败
I (5021) LcdChat: Dirty flag detected, refreshing...  ← 每270ms触发一次
```

**修复方案**：
1. 添加 `wake_word_detector_is_ready()` 函数检测模型是否加载成功
2. 如果模型未加载，任务进入空闲模式（10秒延迟）
3. 正常情况下，`wake_word_wait()` 返回false后添加100ms延迟

```c
static void wake_word_task(void *arg) {
    bool wake_word_ready = wake_word_detector_is_ready();
    
    if (!wake_word_ready) {
        ESP_LOGW(TAG, "Wake word not available, task idle");
        lcd_chat_set_state(&g_lcd, CHAT_STATE_IDLE, "Wake word N/A");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));  // 空闲等待
        }
    }
    
    while (true) {
        // ... 正常逻辑
        if (!wake_word_wait()) {
            vTaskDelay(pdMS_TO_TICKS(100));  // 防止死循环
            continue;
        }
    }
}
```

---

### 问题2：音频爆破音

**现象**：喇叭发出电流爆破音

**根本原因**：蜂鸣器和功放共用 GPIO 46，`buzzer_beep()` 函数会开关功放引脚

```c
// 原始代码
void buzzer_beep(int times) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(s_gpio, 1);  // 开功放 → 爆音
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(s_gpio, 0);  // 关功放 → 爆音
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

**修复方案**：删除蜂鸣器功能

1. 删除 `buzzer.c` 和 `buzzer.h` 文件
2. 从 `CMakeLists.txt` 移除 `buzzer.c`
3. 从 `main.c` 移除所有 `buzzer_beep()` 调用

---

### 问题3：WiFi初始化崩溃

**现象**：程序启动时WiFi初始化失败，触发panic

```
ESP_ERROR_CHECK failed: esp_err_t 0x1101 (ESP_ERR_NVS_NOT_INITIALIZED)
```

**根本原因**：NVS（非易失性存储）未初始化，WiFi依赖NVS存储校准数据

**修复方案**：在WiFi初始化前添加NVS初始化

```c
#include "nvs_flash.h"

bool wifi_manager_connect(...) {
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // ... WiFi初始化代码
}
```

同时需要在 `CMakeLists.txt` 添加依赖：
```cmake
REQUIRES ... nvs_flash
```

---

### 问题4：屏幕残影

**现象**：刷新屏幕时底部有残留图像

**根本原因**：单帧缓冲区在被SPI DMA读取的同时被渲染函数修改

**修复方案**：实现双缓冲机制

```c
typedef struct {
    uint16_t *back_buf;   // 渲染缓冲
    uint16_t *front_buf;  // 显示缓冲
} lcd_display_t;

void lcd_refresh(lcd_display_t *lcd) {
    // 1. 渲染到 back_buf
    draw_header(lcd);
    draw_chat_area(lcd);
    draw_footer(lcd);
    
    // 2. 交换缓冲区
    uint16_t *tmp = lcd->back_buf;
    lcd->back_buf = lcd->front_buf;
    lcd->front_buf = tmp;
    
    // 3. 发送 front_buf 到LCD
    // ...
}
```

---

### 问题5：颜色显示错误（紫色和绿色）

**现象**：字体和界面颜色显示为紫色和绿色，而非预期颜色

**根本原因**：ILI9341 LCD通过SPI传输需要大端序，ESP32是小端序

**修复方案**：在写入帧缓冲时进行字节交换

```c
static inline uint16_t swap_color(uint16_t c) {
    return (c >> 8) | (c << 8);
}

static inline void put_pixel(lcd_display_t *lcd, int x, int y, uint16_t color) {
    lcd->back_buf[y * LCD_WIDTH + x] = swap_color(color);
}
```

---

### 问题6：字体显示反向

**现象**：字体字符左右镜像

**根本原因**：LCD像素写入方向与字体位图读取方向不匹配

**修复方案**：使用硬件镜像

```c
// lcd_init 中
ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd->panel, true, false));  // X轴水平镜像
```

---

### 问题7：I2S采样率冲突

**现象**：串口报错 `Current mode record conflict sample_rate 16000 with peer mode sample_rate 44100`

**原因**：录音（16kHz）和播放（44.1kHz）使用同一个I2S_NUM_0，采样率不同

**当前状态**：未完全解决，但不影响基本功能

---

### 问题8：看门狗触发

**现象**：Task watchdog每5秒触发一次

```
E (8352) task_wdt: Task watchdog got triggered
E (8352) task_wdt:  - IDLE0 (CPU 0)
E (8352) task_wdt: Tasks currently running:
E (8352) task_wdt: CPU 0: wake_word
```

**根本原因**：`wake_word_task` 死循环导致CPU0的idle任务被饿死

**修复方案**：同问题1的修复，添加延迟防止死循环

---

## 关键设计决策

### 1. 双缓冲机制
- **back_buf**：用于渲染，所有绘图操作写入此缓冲
- **front_buf**：用于显示，正在被LCD读取的内容
- 刷新时交换指针，避免撕裂和残影

### 2. 脏标记机制
- 所有API函数只设置 `dirty = true`，不立即刷新
- 后台刷新任务每200ms检查一次脏标记
- 避免频繁刷新导致的闪烁

### 3. 任务核心分配
| 任务 | 核心 | 优先级 |
|------|------|--------|
| wake_word_task | Core 0 | 8 |
| record_task | Core 0 | 7 |
| button_task | Core 0 | 6 |
| lcd_refresh_task | Core 1 | 1 |

### 4. SPI传输优化
- 使用小DMA缓冲区（20行 = 12800字节）
- 每批传输后延迟2ms，让出CPU给音频任务
- 与小智项目的LVGL buffer_size配置一致

---

## 文件修改清单

| 文件 | 修改内容 |
|------|---------|
| `main/lcd_display.h` | 添加双缓冲结构、聊天UI API |
| `main/lcd_display.c` | 实现聊天界面、双缓冲、脏标记机制 |
| `main/main.c` | 适配新API、修复任务逻辑 |
| `main/wifi_manager.c` | 添加NVS初始化 |
| `main/wake_word_detector.h/c` | 添加 `is_ready()` 函数 |
| `main/CMakeLists.txt` | 移除buzzer、添加nvs_flash依赖 |
| `main/idf_component.yml` | 移除esp_jpeg依赖 |
| `main/buzzer.c/h` | 已删除 |

---

## 小智项目参考

小智项目的显示和音频共存方案：

1. **LVGL图形库**：只刷新变化区域，避免全屏刷新
2. **核心隔离**：音频在Core 0，显示在Core 1
3. **优先级分离**：音频优先级8，LVGL优先级1
4. **独立DMA通道**：SPI和I2S使用不同的GDMA通道
5. **部分缓冲**：LVGL buffer_size = width * 20像素

---

## 串口调试命令

```bash
# 激活ESP-IDF环境
source ~/.espressif/v6.0.1/esp-idf/export.sh

# 编译、烧录、监控
idf.py -p /dev/tty.usbmodem1101 flash monitor

# 退出监控
Ctrl+]
```

---

## 问题8：`esp_codec_dev_read()` 导致系统完全冻结

**现象**：`wake_word_task` 启动后，LCD 刷新停止，串口无输出，整个系统卡死。甚至 `i2s_channel_read()` 带超时也无法返回。

**根本原因**：`esp_codec_dev` 框架内部的 `_i2s_data_set_fmt()` 在 `esp_codec_dev_open()` 时会重新配置 I2S RX 的 DMA 参数。当 speaker 和 mic 共享同一个 `data_if` 时，speaker 的 `esp_codec_dev_open()` 会影响 RX DMA 状态，导致后续 `i2s_channel_read()` 在 DMA 层面无限阻塞。

**解决方案**：
1. **不用 `esp_codec_dev` 管理 mic 的 I2S 路径**——只用 I2C 配置 ES7243E（通过 `es7243e_codec_new()`），直接用 `i2s_channel_read()` 读取音频
2. **speaker 和 mic 不共享 `data_if`**——speaker 用独立的 `data_if`（只传 TX handle），mic 不创建 `data_if`
3. **RX 用 STD STEREO 模式**（不是 TDM）——TDM 模式下 `i2s_channel_read()` 返回 error 263

```c
// speaker: STD MONO (只用 dout)
i2s_std_config_t tx_cfg = { ... I2S_SLOT_MODE_MONO ... din = I2S_GPIO_UNUSED };
// RX: STD STEREO (只用 din)  
i2s_std_config_t rx_cfg = { ... I2S_SLOT_MODE_STEREO ... dout = I2S_GPIO_UNUSED };

// ES7243E 只通过 I2C 配置，不走 esp_codec_dev 的 I2S 路径
es7243e_codec_new(&es7243_cfg);  // 内部调用 open() 配置寄存器（含 +30dB 增益）

// 直接用 i2s_channel_read 读取
i2s_channel_read(s_rx_handle, data, size, &bytes_read, 100);
```

---

## 问题9：Opus 解码 error -5

**现象**：WebSocket 接收到 Opus 音频帧后，`esp_opus_dec_decode()` 返回 -5（ESP_AUDIO_ERR_FAIL），日志显示 "Invalid parameter 'opus information'"。

**根本原因**：`esp_opus_dec` 封装层对裸 Opus 帧有额外格式要求，与服务器端 `libopus` 的 `opus_encode()` 输出不兼容。

**解决方案**：绕过 `esp_opus_dec` 封装，直接调用 `esp_audio_codec` 内置的原生 `libopus` API：

```c
// 声明 libopus 函数签名（已包含在 esp_audio_codec 的 .a 库中）
int opus_decoder_get_size(int channels);
int opus_decoder_init(OpusDecoder *st, int Fs, int channels);
int opus_decode(OpusDecoder *st, const unsigned char *data, int len, short *pcm, int frame_size, int decode_fec);

// 初始化
int size = opus_decoder_get_size(1);
s_decoder = (OpusDecoder *)malloc(size);
opus_decoder_init(s_decoder, 16000, 1);

// 解码
int decoded = opus_decode(s_decoder, opus_data, opus_size, pcm_out, pcm_max_samples, 0);
```

---

## 问题10：AFE ringbuffer 溢出/为空

**现象**：唤醒词检测启动后，大量警告 "Ringbuffer of AFE(FEED) is full" 或 "Ringbuffer of AFE is empty"。

**根本原因**：`feed()` 和 `fetch_with_delay()` 在同一个循环中运行。`i2s_channel_read()` 读完立即返回（DMA 有数据），导致 feed 远超实时速率。而 `fetch_with_delay(0)` 非阻塞，立即返回消耗所有数据。

**解决方案**：**双任务架构**（与 xiaozhi 一致）——feed 和 fetch 在独立任务中运行：

```c
// 音频输入任务：读取 I2S 数据并 feed 给 AFE
static void audio_feed_task(void *arg) {
    while (!s_wake_detected) {
        audio_codec_input(s_codec, buf, chunk_samples);
        s_afe->feed(s_afe_data, buf);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

// 主循环：阻塞等待 AFE 处理结果
while (!s_wake_detected) {
    afe_fetch_result_t *result = s_afe->fetch_with_delay(s_afe_data, portMAX_DELAY);
    if (result && result->wakeup_state == WAKENET_DETECTED) {
        // 唤醒词检测到
    }
}
```

---

## 问题11：唤醒词无法检测到

**现象**：AFE 正常运行，Mic RMS 值正常，但唤醒词 "你好小智" 始终无法触发。

**根本原因**：AFE 配置使用 `"M"`（单麦克风通道），但 ES7243E 输出 stereo 2 通道数据。我们提取单声道喂给 AFE，但 xiaozhi 使用 `"MM"`（2 麦克风通道）并直接喂 interleaved stereo 数据。

**解决方案**：
1. AFE 配置改为 `"MM"`（2 通道）
2. 直接喂 interleaved stereo 数据，不提取 mono

```c
// 配置 AFE 为 2 通道
afe_config_t *afe_config = afe_config_init("MM", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);

// feed 时直接喂 stereo 数据
int chunk_samples = feed_chunksize * 2;  // 2ch
s_afe->feed(s_afe_data, stereo_buf);     // 不提取 mono
```

---

## 问题12：唤醒词检测后栈溢出崩溃

**现象**：唤醒词 "你好小智" 成功检测到后，设备立即崩溃重启，日志显示 "A stack overflow in task stream has been detected"。

**根本原因**：`audio_recorder.c` 中的 `stream_task` 栈只有 8192 字节，Opus 编码（`app_opus_encoder_encode()`）需要更多栈空间。

**解决方案**：增大 `TASK_RECORD_STACK_SIZE` 到 32768：

```c
// config.h
#define TASK_RECORD_STACK_SIZE     32768
```

---

## 问题13：`stream_play_task` 队列分配失败

**现象**：唤醒词检测后，日志显示 "Failed to create stream queue"，随后栈溢出崩溃。

**根本原因**：`pcm_frame_t` 结构体为 1924 字节（960 个 int16_t + size_t），队列深度 64 需要 ~123KB 内部 RAM，超出可用内存。

**解决方案**：减小队列深度到 8，或延迟启动 `stream_play_task` 到唤醒词检测后：

```c
#define STREAM_QUEUE_DEPTH 8  // 从 64 减到 8
```

---

## 最终架构总结

### 音频初始化顺序
1. 创建 I2S TX/RX 通道（`i2s_new_channel`）
2. TX: STD MONO 模式（只用 dout）
3. RX: STD STEREO 模式（只用 din）
4. 启用 TX 和 RX
5. ES8156 DAC I2C 初始化（`es8156_codec_new`）
6. ES7243E ADC I2C 初始化（`es7243e_codec_new`，内部调用 `open()` 配置寄存器）

### 唤醒词检测架构
- **afe_feed 任务**（core 1, priority 4）：读取 I2S stereo 数据 → feed 给 AFE
- **主任务**：`fetch_with_delay(portMAX_DELAY)` 阻塞等待 AFE 处理结果

### 关键配置
| 参数 | 值 |
|------|-----|
| AFE 输入格式 | `"MM"`（2 麦克风通道） |
| RX I2S 模式 | STD STEREO |
| TX I2S 模式 | STD MONO |
| ES7243E 增益 | +30dB（`es7243e_codec_new` 默认） |
| stream_task 栈 | 32768 字节 |
| Opus 解码 | 原生 libopus API |
