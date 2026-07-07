/**
 * @file lcd_display.h
 * @brief LCD 显示模块接口 — ILI9341 (320x240) 聊天 UI
 *
 * 功能：
 *   - 聊天界面：消息气泡（用户/AI/系统/错误）、滚动、状态栏
 *   - 连接状态指示：WiFi、MQTT
 *   - 双缓冲 + 脏标记刷新
 *   - 绘图原语：矩形、圆角矩形、像素、线段
 */

#ifndef _LCD_DISPLAY_H_
#define _LCD_DISPLAY_H_

#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 颜色定义 (RGB565) ====================
#define COLOR_BLACK        0x0000
#define COLOR_WHITE        0xFFFF
#define COLOR_RED          0xF800
#define COLOR_GREEN        0x07E0
#define COLOR_BLUE         0x001F
#define COLOR_YELLOW       0xFFE0
#define COLOR_CYAN         0x07FF
#define COLOR_MAGENTA      0xF81F
#define COLOR_GRAY         0x8410
#define COLOR_DARK_GRAY    0x4208
#define COLOR_LIGHT_GRAY   0xC618
#define COLOR_ORANGE       0xFD20
#define COLOR_DARK_BG      0x10A2      // 深色背景
#define COLOR_HEADER_BG    0x2124      // 头部背景
#define COLOR_STATUS_BG    0x18E3      // 状态栏背景
#define COLOR_USER_BUBBLE  0x2D5B      // 用户气泡 (深蓝)
#define COLOR_AI_BUBBLE    0x2C8B      // AI 气泡 (深绿)
#define COLOR_BUBBLE_BORDER 0x4A69     // 气泡边框
#define COLOR_ACCENT       0x0C7F      // 强调色 (蓝绿)

// ==================== 布局常量 ====================
#define HEADER_HEIGHT      28          // 头部高度
#define FOOTER_HEIGHT      24          // 底部状态栏高度
#define CHAT_AREA_Y        HEADER_HEIGHT
#define CHAT_AREA_HEIGHT   (LCD_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT)
#define BUBBLE_PADDING     6           // 气泡内边距
#define BUBBLE_RADIUS      4           // 气泡圆角半径
#define MSG_SPACING        4           // 消息间距
#define TEXT_MARGIN_X      8           // 文字左边距
#define BUBBLE_MAX_WIDTH   240         // 气泡最大宽度
#define BUBBLE_MIN_WIDTH   40          // 气泡最小宽度

// ==================== 消息类型 ====================
typedef enum {
    MSG_TYPE_USER,      // 用户消息 (右对齐)
    MSG_TYPE_AI,        // AI 回复 (左对齐)
    MSG_TYPE_SYSTEM,    // 系统消息 (居中)
    MSG_TYPE_ERROR,     // 错误消息 (居中，红色)
} msg_type_t;

// ==================== 消息结构 ====================
typedef struct {
    msg_type_t type;
    char text[256];     // 消息文本
    uint32_t timestamp; // 时间戳 (可选)
} chat_msg_t;

// ==================== 聊天状态 ====================
typedef enum {
    CHAT_STATE_IDLE,        // 空闲等待
    CHAT_STATE_LISTENING,   // 正在听
    CHAT_STATE_THINKING,    // 思考中
    CHAT_STATE_SPEAKING,    // 播报中
    CHAT_STATE_ERROR,       // 错误状态
} chat_state_t;

// ==================== 显示上下文 ====================
#define MAX_CHAT_MESSAGES  20  // 最大消息数

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel;
    gpio_num_t bl_pin;
    bool initialized;

    // 聊天消息历史
    chat_msg_t messages[MAX_CHAT_MESSAGES];
    int msg_count;
    int msg_head;       // 环形缓冲区头
    int scroll_offset;  // 滚动偏移 (0 = 最新消息)

    // 当前状态
    chat_state_t state;
    char status_text[64];

    // 连接状态
    bool wifi_connected;
    bool mqtt_connected;

    // 双缓冲：back用于渲染，front用于显示
    uint16_t *back_buf;
    uint16_t *front_buf;

    // 脏标记
    volatile bool dirty;
} lcd_display_t;

// ==================== 初始化 ====================
bool lcd_init(lcd_display_t *lcd,
              gpio_num_t cs, gpio_num_t dc, gpio_num_t rst,
              gpio_num_t mosi, gpio_num_t sclk, gpio_num_t bl);

void lcd_set_backlight(lcd_display_t *lcd, bool on);

// 启动后台刷新任务（在lcd_init之后调用）
void lcd_start_refresh_task(lcd_display_t *lcd);

// ==================== 聊天界面 API ====================

// 添加消息到聊天历史
void lcd_chat_add_message(lcd_display_t *lcd, msg_type_t type, const char *text);

// 设置当前状态
void lcd_chat_set_state(lcd_display_t *lcd, chat_state_t state, const char *detail);

// 设置连接状态
void lcd_set_wifi_status(lcd_display_t *lcd, bool connected);
void lcd_set_mqtt_status(lcd_display_t *lcd, bool connected);

// 滚动控制
void lcd_chat_scroll_up(lcd_display_t *lcd);
void lcd_chat_scroll_down(lcd_display_t *lcd);
void lcd_chat_scroll_to_bottom(lcd_display_t *lcd);

// 刷新整个界面
void lcd_refresh(lcd_display_t *lcd);

// 清空聊天历史
void lcd_chat_clear(lcd_display_t *lcd);

// ==================== 便捷函数 (兼容旧接口) ====================
void lcd_show_ready(lcd_display_t *lcd);
void lcd_show_status(lcd_display_t *lcd, const char *status);
void lcd_show_reply(lcd_display_t *lcd, const char *user, const char *text);
void lcd_show_identity(lcd_display_t *lcd, const char *user, float confidence);
void lcd_show_alert(lcd_display_t *lcd, const char *msg);
void lcd_show_error(lcd_display_t *lcd, const char *msg);
void lcd_show_now_playing(lcd_display_t *lcd, const char *track, const char *artist, const char *state);
void lcd_show_music_error(lcd_display_t *lcd, const char *msg);
void lcd_show_skill(lcd_display_t *lcd, const char *user, const char *skill_name);

// ==================== 绘图原语 ====================
void lcd_fill_color(lcd_display_t *lcd, uint16_t color);
void lcd_draw_rect(lcd_display_t *lcd, int x, int y, int w, int h, uint16_t color);
void lcd_draw_rect_filled(lcd_display_t *lcd, int x, int y, int w, int h, uint16_t color);
void lcd_draw_rounded_rect(lcd_display_t *lcd, int x, int y, int w, int h, int r, uint16_t color);
void lcd_draw_pixel(lcd_display_t *lcd, int x, int y, uint16_t color);
void lcd_draw_hline(lcd_display_t *lcd, int x, int y, int w, uint16_t color);
void lcd_draw_vline(lcd_display_t *lcd, int x, int y, int h, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif // _LCD_DISPLAY_H_
