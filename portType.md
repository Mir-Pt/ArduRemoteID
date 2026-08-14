# RemoteID 接口引脚定义

> ESP32-C3 RemoteID 接口引脚定义

---

## 1. USB

| PIN | TYPE | GPIO   | Peripheral      | 说明                       |
| --- | ---- | ------ | --------------- | ------------------------ |
| 1   | +5V  | +5V    | —               | USB VBUS 电源输入（5V）        |
| 2   | D-   | GPIO18 | USB-Serial-JTAG | USB 差分数据负端（D-），接电脑 USB 口 |
| 3   | D+   | GPIO19 | USB-Serial-JTAG | USB 差分数据正端（D+），接电脑 USB 口 |
| 4   | GND  | GND    | —               | 地                        |

## 2. UART0 接口（调试串口）

| PIN | TYPE  | GPIO | Peripheral | 说明                                     |
| --- | ----- | ---- | ---------- | -------------------------------------- |
| 1   | +5V   | +5V  | —          | 电源输入                                   |
| 2   | U0_TX | TXD  | UART0      | ESP32-C3 UART0 发送（U0TXD，GPIO21），调试日志输出 |
| 3   | U0_RX | RXD  | UART0      | ESP32-C3 UART0 接收（U0RXD，GPIO20），调试命令输入 |
| 4   | GND   | GND  | —          | 地                                      |

## 3. UART1 MavLink 接口

> 🔧 **修改记录**（直通线接飞控 TELEM2）：Pin2/Pin3 与飞控 TX/RX 方向相反，固件端口映射已交换 ——
> `PIN_UART_TX` 3→10、`PIN_UART_RX` 10→3。GPIO 焊接不变（Pin2=GPIO3、Pin3=GPIO10）。

| PIN | TYPE            | GPIO   | Peripheral | 说明                        |
| --- | --------------- | ------ | ---------- | ------------------------- |
| 1   | +5V             | +5V    | —          | 电源输入                      |
| 2   | ~~U1_TX~~ U1_RX | GPIO3  | UART1      | 模块接收 ← 飞控 TX（TELEM2 Pin2） |
| 3   | ~~U1_RX~~ U1_TX | GPIO10 | UART1      | 模块发送 → 飞控 RX（TELEM2 Pin3） |
| 4   | NC              | NC     | —          | 未连接                       |
| 5   | NC              | NC     | —          | 未连接                       |
| 6   | GND             | GND    | —          | 地                         |

## 4. CAN 接口（连接器）

| PIN | TYPE  | GPIO  | Peripheral  | 说明                     |
| --- | ----- | ----- | ----------- | ---------------------- |
| 1   | +5V   | +5V   | —           | 电源输入                   |
| 2   | CAN_H | CAN_H | TJA1051 收发器 | CAN 总线差分高电平，来自收发器 pin7 |
| 3   | CAN_L | CAN_L | TJA1051 收发器 | CAN 总线差分低电平，来自收发器 pin6 |
| 4   | GND   | GND   | —           | 地                      |

## 5. CAN 接口（芯片侧）

| PIN | TYPE   | GPIO  | Peripheral | 说明                                      |
| --- | ------ | ----- | ---------- | --------------------------------------- |
| 1   | CAN_RX | GPIO4 | TWAI       | ESP32-C3 CAN 控制器接收，接 TJA1051 RXD (pin4) |
| 2   | CAN_TX | GPIO5 | TWAI       | ESP32-C3 CAN 控制器发送，接 TJA1051 TXD (pin1) |

## 6. LED PWM 接口（2 棵 SK6805-EC15）

| PIN | TYPE    | GPIO  | Peripheral | 说明                                       |
| --- | ------- | ----- | ---------- | ---------------------------------------- |
| 1   | LED_PWM | GPIO8 | RMT        | WS2812 兼容协议，2 颗 SK6805-EC15 级联（DIN→DOUT） |

---
