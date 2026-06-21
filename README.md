# ESP32-S3-BOX-Lite 智能管家终端

基于 ESP32-S3-BOX-Lite 开发板的多功能智能终端固件，集成音频播放/采集、LCD 显示和 Codec 基准测试。

## 硬件平台

| 组件 | 型号 | 说明 |
|------|------|------|
| 主控 | ESP32-S3 | 双核 Xtensa LX7, 8MB PSRAM |
| DAC (扬声器) | ES8156 | I2C 地址 0x10, I2S 输出 |
| ADC (麦克风) | ES7243E | I2C 地址 0x20, I2S 输入 |
| 功放 | NS4150 | GPIO46 控制 (HIGH=开) |
| 显示屏 | ILI9341 | 320x240, SPI 接口 |
| 按键 | ADC 按键 x3 | GPIO1 ADC 多路复用 |

## 引脚定义

### I2S 音频

| 信号 | GPIO | 说明 |
|------|------|------|
| MCLK | 2 | 主时钟 |
| BCLK | 17 | 位时钟 |
| WS | 47 | 字选择 (LRCK) |
| DOUT | 15 | 数据输出 → ES8156 |
| DIN | 16 | 数据输入 ← ES7243E |

### I2C 控制

| 信号 | GPIO | 说明 |
|------|------|------|
| SDA | 8 | 数据线 |
| SCL | 18 | 时钟线 |

### LCD (SPI)

| 信号 | GPIO | 说明 |
|------|------|------|
| CS | 5 | 片选 |
| DC | 4 | 数据/命令 |
| RST | 48 | 复位 |
| MOSI | 6 | 主出从入 |
| SCLK | 7 | SPI 时钟 |
| BL | 45 | 背光 (LOW=亮) |

### 功放控制

| 信号 | GPIO | 说明 |
|------|------|------|
| PA_EN | 46 | HIGH=开启功放 |

### ADC 按键

三个按键共用 ADC1_CH0 (GPIO1)，通过不同电压分压区分：

| 按键 | 电压范围 (mV) | 功能 |
|------|---------------|------|
| 左键 (PREV) | 2310 - 2510 | 音量 -10% |
| 中键 (ENTER) | 1880 - 2080 | 运行 Benchmark |
| 右键 (NEXT) | 720 - 920 | 音量 +10% |

## 功能模块

### 1. 音频播放 (ES8156 DAC)

- I2S 主模式, 44100Hz, 16-bit 立体声
- 功放 GPIO46=HIGH 开启
- 音量范围 0-100%, 默认 50%
- 支持 MP3 解码播放 (esp_audio_codec)

```c
audio_codec_init(&codec, i2c_bus, PA_PIN, MCLK, BCLK, WS, DOUT, DIN);
audio_codec_enable_output(&codec, true);
audio_codec_set_volume(&codec, 50);
audio_codec_output(&codec, pcm_data, samples);
```

### 2. 麦克风采集 (ES7243E ADC)

- I2S 主模式, 16000Hz, 16-bit 立体声
- 支持原始 PCM 录制和 RMS 电平检测

```c
audio_codec_enable_input(&codec, true);
audio_codec_input(&codec, buf, samples);         // 原始 PCM
int rms = audio_codec_detect_voice(&codec, 200); // RMS 电平
```

**语音检测阈值:**

| RMS 范围 | 判定 |
|----------|------|
| < 200 | 安静 / 无声音 |
| 200 - 500 | 微弱声音 / 环境噪声 |
| > 500 | 有明显声音 |
| > 2000 | 较大声音 |

### 3. Codec Benchmark

综合测试扬声器和麦克风功能，自动评估音频通路状态。

```c
benchmark_result_t result;
codec_benchmark_run(&codec, &lcd, &result);
codec_benchmark_print_result(&result);
```

**测试流程:**

1. **环境噪声测量** - 录制 3 秒静默环境，计算 RMS 基线
2. **扬声器播放** - 播放 1000Hz 正弦波测试音调 (3 秒)
3. **回环采集** - 播放同时麦克风录制，检测声音回环
4. **麦克风单独测试** - 录制 3 秒环境，验证麦克风功能

**结果结构:**

```c
typedef struct {
    int  tone_rms;       // 回环 RMS 电平
    int  silence_rms;    // 环境噪声 RMS
    int  mic_rms;        // 麦克风单独 RMS
    bool tone_detected;  // 回环检测通过
    bool mic_ok;         // 麦克风正常
    bool spk_ok;         // 扬声器正常
} benchmark_result_t;
```

### 4. LCD 显示

- ILI9341 驱动, 320x240, 16-bit 色彩
- SPI 接口, 40MHz 时钟
- 支持测试图案和 JPEG 图片显示

```c
lcd_init(&lcd, CS, DC, RST, MOSI, SCLK, BL);
lcd_test_pattern(&lcd);
lcd_display_jpeg(&lcd, jpeg_data, jpeg_size);
```

## 操作说明

| 操作 | 说明 |
|------|------|
| 按左键 | 音量减小 10% |
| 按右键 | 音量增大 10% |
| 按中键 | 运行 Codec Benchmark 测试 |
| 上电自动 | 播放 MP3 → 运行 Benchmark → 显示 JPEG |

## 编译与烧录

```bash
# 激活 ESP-IDF 环境
source ~/.espressif/tools/activate_idf_v6.0.1.sh

# 编译
idf.py build

# 烧录
idf.py -p /dev/cu.usbmodem101 flash

# 监控串口
idf.py -p /dev/cu.usbmodem101 monitor
```

## 项目结构

```
esp32_box_lite/
├── main/
│   ├── main.c              # 主程序, 按键控制, MP3 播放
│   ├── audio_codec.c/h     # 音频编解码器 (ES8156 + ES7243E)
│   ├── codec_benchmark.c/h # Codec 基准测试
│   ├── lcd_display.c/h     # LCD 显示驱动
│   ├── config.h            # 硬件引脚和参数配置
│   ├── CMakeLists.txt      # 组件注册
│   ├── idf_component.yml   # 依赖管理
│   └── test.jpg            # 测试图片
├── output.mp3              # 测试音频
├── CMakeLists.txt          # 项目配置
├── sdkconfig               # ESP-IDF 配置
└── partitions.csv          # 分区表
```

## 依赖组件

| 组件 | 版本 | 用途 |
|------|------|------|
| espressif/esp_lcd_ili9341 | ~2.0.0 | LCD 驱动 |
| espressif/esp_codec_dev | ~1.5.6 | 音频 Codec 设备框架 |
| espressif/esp_audio_codec | ~2.4.1 | MP3 解码器 |
| espressif/esp_jpeg | ^1.0.0 | JPEG 解码 |
