/*
  LED 状态指示灯控制实现

  根据板级配置自动选择 GPIO LED 或 WS2812 RGB LED：
  - GPIO LED：解锁就绪时保持 STATUS_LED_OK 电平，否则以 100ms 间隔闪烁
  - WS2812 LED：解锁就绪时显示绿色，否则显示红色，每 200ms 刷新一次
 */
#include <Arduino.h>
#include "led.h"
#include "board_config.h"

// 全局 LED 控制实例
Led led;

/*
  init() - LED 硬件初始化
  配置 LED 引脚模式，初始化 WS2812 灯带。仅执行一次。
 */
void Led::init(void)
{
    if (done_init) {
        return;
    }
    done_init = true;
#ifdef PIN_STATUS_LED
    pinMode(PIN_STATUS_LED, OUTPUT);    // 配置 GPIO LED 引脚为输出
#endif
#ifdef WS2812_LED_PIN
    pinMode(WS2812_LED_PIN, OUTPUT);    // 配置 WS2812 数据引脚为输出
    ledStrip.begin();                   // 初始化 NeoPixel 灯带
#endif
}

/*
  update() - 更新 LED 显示状态
  需要在主循环中周期调用。根据当前状态控制 LED 亮灭/颜色。
 */
void Led::update(void)
{
    init();

    const uint32_t now_ms = millis();

    // ==================== GPIO LED 控制 ====================
#ifdef PIN_STATUS_LED
    switch (state) {
    case LedState::ARM_OK: {
        // 解锁就绪：LED 保持 STATUS_LED_OK 指定的电平（常亮或常灭，取决于板型）
        digitalWrite(PIN_STATUS_LED, STATUS_LED_OK);
        last_led_trig_ms = now_ms;
        break;
    }

    default:
        // 其他状态（初始化/自检失败/解锁失败）：LED 以 100ms 间隔快速闪烁
        if (now_ms - last_led_trig_ms > 100) {
            digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
            last_led_trig_ms = now_ms;
        }
        break;
    }
#endif

    // ==================== WS2812 RGB LED 控制 ====================
#ifdef WS2812_LED_PIN
    ledStrip.clear();   // 清除所有像素颜色

    // 各状态的颜色（RGB）与闪烁周期（0 = 不闪烁）
    uint8_t r = 0, g = 0, b = 0;
    uint32_t blink_ms = 0;

    switch (state) {
    case LedState::INIT:
        // 初始化中：蓝色常亮
        b = 255;
        break;
    case LedState::PFST_FAIL:
        // 开机自检失败：黄色快闪
        r = 255; g = 255;
        blink_ms = 250;
        break;
    case LedState::ARM_FAIL:
        // 解锁失败：红色慢闪
        r = 255;
        blink_ms = 500;
        break;
    case LedState::ARM_OK:
        // 解锁就绪：绿色常亮
        g = 255;
        break;
    }

    // 闪烁相位：blink_ms 周期内前半亮、后半灭
    if (blink_ms != 0 && ((now_ms / blink_ms) & 1) == 0) {
        r = g = b = 0;
    }

    const uint32_t color = ledStrip.Color(r, g, b);
    ledStrip.setPixelColor(0, color);   // LED 1
    ledStrip.setPixelColor(1, color);   // LED 2（双 LED 保持一致）

    // 每 50ms 刷新一次（保证 250ms 快闪可见）
    if (now_ms - last_led_strip_ms >= 50) {
        last_led_strip_ms = now_ms;
        ledStrip.show();    // 将颜色数据发送到 LED 硬件
    }
#endif
}

