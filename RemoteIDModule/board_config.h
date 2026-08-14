/*
  硬件板级配置文件

  本文件通过条件编译定义各种硬件板型的引脚分配和外设配置。
  每种板型需要定义：
  - BOARD_ID：板型唯一标识号
  - PIN_CAN_TX/RX：CAN 总线收发引脚（DroneCAN 通信）
  - PIN_UART_TX/RX：UART 串口引脚（MAVLink 通信）
  - LED 引脚：WS2812 RGB LED 或普通 GPIO LED
  - 可选：CAN 使能/静默/终端电阻引脚、蜂鸣器引脚等

  当前激活的板型由下方的 #define BOARD_xxx 宏决定。
  编译时只有匹配的板型配置会生效。
 */

#pragma once

// ==================== 当前激活的板型 ====================
#define BOARD_ESP32C3_DEV

// ==================== 板型 1：ESP32-S3 开发板 ====================
#ifdef BOARD_ESP32S3_DEV
#define BOARD_ID 1
#define PIN_CAN_TX GPIO_NUM_47      // CAN 发送引脚
#define PIN_CAN_RX GPIO_NUM_38      // CAN 接收引脚

#define PIN_UART_TX 18              // UART 发送引脚（连接飞控）
#define PIN_UART_RX 17              // UART 接收引脚

#define WS2812_LED_PIN GPIO_NUM_48  // WS2812 RGB LED 数据引脚

// ==================== 板型 2：ESP32-C3 开发板 ====================
#elif defined(BOARD_ESP32C3_DEV)
#define BOARD_ID 2
#define PIN_CAN_TX GPIO_NUM_5
#define PIN_CAN_RX GPIO_NUM_4

#define PIN_UART_TX 10   // UART 发送：GPIO10 → 连接器 Pin3 → 飞控 RX（直通线）
#define PIN_UART_RX 3    // UART 接收：GPIO3  → 连接器 Pin2 ← 飞控 TX（直通线）

#define WS2812_LED_PIN GPIO_NUM_8

// ==================== 板型 3：BlueMark DB200 ====================
#elif defined(BOARD_BLUEMARK_DB200)
#define BOARD_ID 3

#define PIN_CAN_TX GPIO_NUM_0       // CAN TX → NXP CAN 收发器的 TX（输入）引脚
#define PIN_CAN_RX GPIO_NUM_5       // CAN 接收引脚
#define PIN_CAN_EN GPIO_NUM_4       // NXP CAN 收发器使能引脚
#define PIN_CAN_nSILENT GPIO_NUM_1  // CAN 静默模式引脚（低电平有效）

#define CAN_APP_NODE_NAME "BlueMark DB200"  // DroneCAN 节点名称

// 如需启用 CAN 总线终端电阻，取消注释下行
//#define PIN_CAN_TERM GPIO_NUM_10  // 终端电阻使能引脚

#define PIN_UART_TX 3
#define PIN_UART_RX 2

#define PIN_STATUS_LED GPIO_NUM_8   // 状态指示 LED 引脚
#define STATUS_LED_OK 0             // LED 熄灭 = 解锁就绪

// ==================== 板型 4：BlueMark DB110 ====================
#elif defined(BOARD_BLUEMARK_DB110)
#define BOARD_ID 4
#define PIN_UART_TX 5
#define PIN_UART_RX 4

#define PIN_STATUS_LED GPIO_NUM_8   // 状态指示 LED 引脚
#define STATUS_LED_OK 0             // LED 熄灭 = 解锁就绪

// ==================== 板型 5：JW TBD ====================
#elif defined(BOARD_JW_TBD)
#define BOARD_ID 5
#define PIN_CAN_TX GPIO_NUM_47
#define PIN_CAN_RX GPIO_NUM_38

#define PIN_UART_TX 18
#define PIN_UART_RX 17

#define PIN_STATUS_LED GPIO_NUM_5   // 状态指示 LED 引脚
#define LED_MODE_FLASH 1            // LED 闪烁模式
#define STATUS_LED_OK 1             // LED 点亮 = 解锁就绪

#define CAN_APP_NODE_NAME "JW TBD"

// ==================== 板型 6：mRo RID ====================
#elif defined(BOARD_MRO_RID)
#define BOARD_ID 6
#define PIN_CAN_TX GPIO_NUM_0
#define PIN_CAN_RX GPIO_NUM_1

#define PIN_UART_TX 4
#define PIN_UART_RX 5

#define WS2812_LED_PIN GPIO_NUM_2
#define CAN_APP_NODE_NAME "mRobotics RemoteID"

// ==================== 板型 7：JW RID ESP32-S3 ====================
#elif defined(BOARD_JWRID_ESP32S3)
#define BOARD_ID 7
#define PIN_CAN_TX GPIO_NUM_47
#define PIN_CAN_RX GPIO_NUM_38

#define PIN_UART_TX 37
#define PIN_UART_RX 36

#define WS2812_LED_PIN GPIO_NUM_5

#define CAN_APP_NODE_NAME "JWRID_ESP32S3"

// ==================== 板型 8：BlueMark DB202 ====================
#elif defined(BOARD_BLUEMARK_DB202)
#define BOARD_ID 8

#define PIN_UART_TX 7
#define PIN_UART_RX 6

#define PIN_STATUS_LED GPIO_NUM_8
#define STATUS_LED_OK 0             // LED 熄灭 = 解锁就绪

// ==================== 板型 10：BlueMark DB210 PRO ====================
#elif defined(BOARD_BLUEMARK_DB210)
#define BOARD_ID 10
#define PIN_CAN_TX GPIO_NUM_19
#define PIN_CAN_RX GPIO_NUM_20

#define PIN_UART_TX 7               // TELEM 1 发送引脚
#define PIN_UART_RX 6               // TELEM 1 接收引脚
//#define PIN_UART_CTS 15           // TELEM 1 CTS（未使用，保留备用）
//#define PIN_UART_RTS 16           // TELEM 1 RTS（未使用，保留备用）

//#define PIN_UART_TX_2 2           // TELEM 2 发送引脚（备用串口）
//#define PIN_UART_RX_2 1           // TELEM 2 接收引脚
//#define PIN_UART_CTS_2 5          // TELEM 2 CTS
//#define PIN_UART_RTS_2 4          // TELEM 2 RTS

#define BUZZER_PIN GPIO_NUM_39      // 有源蜂鸣器引脚（GPIO 高电平 = 蜂鸣）

#define WS2812_LED_PIN GPIO_NUM_8   // WS2812 LED 数据引脚（该 GPIO 上连接 2 颗 LED）

#define CAN_APP_NODE_NAME "BlueMark DB210PRO"
//#define PIN_CAN_TERM GPIO_NUM_42  // CAN 终端电阻使能引脚（取消注释以启用）

// ==================== 板型 9：BlueMark DB203 ====================
#elif defined(BOARD_BLUEMARK_DB203)
#define BOARD_ID 9

#define PIN_CAN_TX GPIO_NUM_6
#define PIN_CAN_RX GPIO_NUM_7

#define PIN_UART_TX 5               // 未实际使用，但需定义以避免编译错误
#define PIN_UART_RX 4               // 未实际使用，但需定义以避免编译错误

#define CAN_APP_NODE_NAME "BlueMark DB203"

#define PIN_STATUS_LED GPIO_NUM_8
#define STATUS_LED_OK 0             // LED 熄灭 = 解锁就绪

// ==================== 板型 11：Holybro RemoteID（当前使用）====================
#elif defined(BOARD_Holybro_RemoteID)
#define BOARD_ID 11
#define PIN_CAN_TX GPIO_NUM_5
#define PIN_CAN_RX GPIO_NUM_4

#define PIN_UART_TX 3
#define PIN_UART_RX 2

#define WS2812_LED_PIN GPIO_NUM_8

// ==================== 板型 12：CUAV C-RID ====================
#elif defined(BOARD_CUAV_RID)
#define BOARD_ID 12
#define PIN_CAN_TX GPIO_NUM_47
#define PIN_CAN_RX GPIO_NUM_38
#define PIN_CAN_nSILENT GPIO_NUM_1  // CAN 静默模式引脚

#define PIN_UART_TX 18
#define PIN_UART_RX 17

#define WS2812_LED_PIN GPIO_NUM_48

#define PIN_CAN_TERM GPIO_NUM_37    // CAN 终端电阻使能引脚
#define CAN_TERM_EN  LOW            // 低电平使能终端电阻（与默认 HIGH 相反）
#define CAN_APP_NODE_NAME "net.cuav.c-rid"

#else
//#error "unsupported board"        // 未定义已知板型时报错（当前已注释）
#endif
