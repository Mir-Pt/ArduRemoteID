/*
  LED 状态指示灯控制模块

  支持两种 LED 硬件：
  1. 普通 GPIO LED（PIN_STATUS_LED）— 通过高低电平控制亮灭
  2. WS2812 RGB LED（WS2812_LED_PIN）— 通过 Adafruit NeoPixel 库驱动

  LED 状态含义（全功能，4 状态 4 色）：
  - 蓝色/常亮：初始化中（INIT）
  - 黄色/快闪：开机自检失败（PFST_FAIL）
  - 红色/慢闪：解锁失败（ARM_FAIL），数据验证未通过或未收到数据
  - 绿色/常亮：解锁就绪（ARM_OK），数据验证通过
 */
#pragma once

#include <stdint.h>
#include "board_config.h"

#ifdef WS2812_LED_PIN
#include <Adafruit_NeoPixel.h>
#endif

// LED 控制类
class Led {
public:
    // LED 状态枚举
    enum class LedState {
        INIT=0,         // 初始化中
        PFST_FAIL,      // 开机自检失败
        ARM_FAIL,       // 解锁失败（红色/闪烁）
        ARM_OK          // 解锁就绪（绿色/常亮）
    };

    // 设置当前 LED 状态
    void set_state(LedState _state) {
        state = _state;
    }

    void update(void);  // 更新 LED 显示（需在主循环中周期调用）

private:
    void init(void);                // LED 硬件初始化
    bool done_init;                 // 是否已完成初始化
    uint32_t last_led_trig_ms;      // 上次 LED 切换的时间戳（用于闪烁控制）
    LedState state;                 // 当前 LED 状态

#ifdef WS2812_LED_PIN
    uint32_t last_led_strip_ms;     // 上次 WS2812 LED 刷新的时间戳
    // WS2812 LED 灯带实例（2 颗 LED，兼容 DB210PRO 双 LED 设计）
    Adafruit_NeoPixel ledStrip{2, WS2812_LED_PIN, NEO_GRB + NEO_KHZ800};
#endif
};

extern Led led;  // 全局 LED 控制实例
