# ESP32-C3-DevKitM-1 调试笔记

> 日期：2026-07-16（最后更新：2026-08-13）

## 一、硬件信息

| 项目     | 值                                   |
| -------- | ------------------------------------ |
| 开发板   | 乐鑫官方 ESP32-C3-DevKitM-1          |
| 芯片     | ESP32-C3 (ECO7)                      |
| 串口     | COM5                                 |
| 板型配置 | `BOARD_ESP32C3_DEV`                |
| 编译工具 | arduino-cli 1.4.1, ESP32 Core 2.0.11 |

## 二、编译与烧写

### 2.1 板型切换

[board_config.h](RemoteIDModule/board_config.h) 中将 `BOARD_Holybro_RemoteID` 改为 `BOARD_ESP32C3_DEV`：

```cpp
#define BOARD_ESP32C3_DEV
```

该板型引脚定义（[board_config.h:33-42](RemoteIDModule/board_config.h#L33-L42)）：

- CAN TX: GPIO 5, CAN RX: GPIO 4
- UART TX: GPIO 3, UART RX: GPIO 2
- WS2812 LED: GPIO 8

### 2.2 编译命令

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3 \
  --libraries "C:/Users/Administrator/Documents/github/ArduRemoteID/libraries" \
  --build-property "build.extra_flags=-DBOARD_ESP32C3_DEV -DESP32" \
  --build-property "build.partitions=partitions" \
  --build-property "upload.maximum_size=2031616" \
  --build-property "build.custom_partitions=C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule/partitions.csv" \
  --upload --port COM5 \
  "C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule"
```

关键差异：`--fqbn` 从 `esp32:esp32:esp32s3` 改为 `esp32:esp32:esp32c3`。

### 2.3 编译结果

```
Sketch uses 1610314 bytes (79%) of program storage space. Maximum is 2031616 bytes.
Global variables use 68204 bytes (20%) of dynamic memory, leaving 259476 bytes for local variables.
```

## 三、固件启动验证

串口 115200 输出确认固件正常运行：

```
ESP-ROM:esp32c3-eco7-20230720
ArduRemoteID version 1.14 07033684
CAN/TWAI Driver installed
CAN/TWAI Driver started
WAP start ArduRemoteID ArduRemoteID
WAP started
Waiting for heartbeat
```

- 固件版本 1.14，commit `07033684`
- CAN 驱动正常启动
- WiFi AP `ArduRemoteID` 正常广播
- "Waiting for heartbeat" 为正常状态（未连接飞控）
- App（嗖嗖FLY 等）搜不到广播设备

## 四、WiFi 连接问题排查

### 4.1 现象

- 笔记本能正常连接 `ArduRemoteID` WiFi 并访问 `http://192.168.4.1`
- 台式机无法连接，两个 WiFi 模块均显示"无法连接这个网络"
- 天线距离很近（排除信号强度问题）

### 4.2 根因

[WiFi_TX.cpp:44](RemoteIDModule/WiFi_TX.cpp#L44) 中 `WiFi.softAP()` 的 `max_connection` 参数设为 1：

```cpp
// 旧代码
WiFi.softAP(g.wifi_ssid, g.wifi_password, g.wifi_channel, false, 1);
```

ESP32 SoftAP 仅允许 1 个客户端连接。笔记本先连上后，台式机就被拒绝了。

### 4.3 修复

将最大连接数从 1 改为 4：

```cpp
// 新代码
WiFi.softAP(g.wifi_ssid, g.wifi_password, g.wifi_channel, false, 4);
```

## 五、RID 广播 App 搜不到设备 — 完整排查记录

### 5.1 现象与初步分析

**用户反馈**：嗖嗖FLY 没有发现广播网络（该 App 通过扫描 WiFi Beacon 中的 Vendor IE 来发现 RID 设备）。

**初步排查思路**：

RID 设备通过以下通道广播：

| 通道           | 默认速率               | 依赖 |
| -------------- | ---------------------- | ---- |
| WiFi NAN       | 0 Hz（关闭）           | —   |
| WiFi Beacon    | 0 Hz（**关闭**） | —   |
| BT4 Legacy     | 1 Hz                   | —   |
| BT5 Long Range | 1 Hz                   | —   |

RID 接收 App（嗖嗖FLY、Drone Scanner 等）**主要通过 WiFi Beacon 的 Vendor IE 帧来发现设备**。WiFi Beacon 默认速率 = 0，没有任何 RID 数据在发。

→ **修复1**：将 `WIFI_BCN_RATE` 默认值从 0 改为 3 Hz（[parameters.cpp:56](RemoteIDModule/parameters.cpp#L56)）。

### 5.2 修复1 无效 — 加调试日志定位

改了默认值、清除 NVS 后重新烧录，嗖嗖FLY 仍然搜不到。

**下一步**：在 `transmit_beacon()` 和 `loop()` 中加调试日志，打印 Beacon 发射状态和 LocationValid 值。

WiFi_TX.cpp 中加入：

```cpp
// 构建 Beacon 帧后打印结果
static uint32_t last_bcn_dbg;
uint32_t nm = millis();
if (nm - last_bcn_dbg > 1000) {
    last_bcn_dbg = nm;
    Serial.printf("BCN: len=%d cnt=%d rate=%.1f payload=%d\n",
        length, send_counter_beacon, g.wifi_beacon_rate, length-58);
}
```

RemoteIDModule.ino 中加入：

```cpp
Serial.printf("BCN: %s rate=%.1f valid=%d\n",
    beacon_ok ? "OK" : "FAIL", g.wifi_beacon_rate, UAS_data.LocationValid);
```

### 5.3 调试输出暴露根因

**串口输出**：

```
BCN: FAIL rate=3.0 valid=0
BCN: FAIL rate=3.0 valid=0
BCN: FAIL rate=3.0 valid=0
...
```

三个关键信息：

- **`FAIL`** — `transmit_beacon()` 返回 false
- **`rate=3.0`** — 参数正确加载（速率确认为 3 Hz）
- **`valid=0`** — `UAS_data.LocationValid == 0`，数据包构建会失败

**既然 `BCAST_POWERUP=1` 会在 `loop()` 里设 `LocationValid=1`，为什么是 0？**

### 5.4 追踪代码流 — 发现执行顺序 Bug

打开 [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino) 追踪 `loop()` 调用链：

```cpp
void loop()
{
    // ... 数据接收、Web 界面更新 ...

    // ===== 步骤1：广播启动条件判断（第468行）=====
    if (g.bcast_powerup) {
        if (!UAS_data.LocationValid) {
            UAS_data.Location.Status = ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE;
            UAS_data.LocationValid = 1;    // ← 这里设了 1
        }
    }

    // ===== 步骤2：填充 UAS_data（第484行）=====
    set_data(transport);  // ← 这里面会重置 LocationValid！

    // ===== 步骤3：广播发射（第497行）=====
    wifi.transmit_beacon(UAS_data);  // ← 读到的 LocationValid 已经是 0
}
```

追踪 `set_data()` 内部（[RemoteIDModule.ino:250](RemoteIDModule/RemoteIDModule.ino#L250)）：

```cpp
static void set_data(Transport &t)
{
    odid_initUasData(&UAS_data);  // ← 把所有字段重置为初始值，LocationValid=0

    // ... 从传输层填充各字段 ...
    // 没有飞控连接 → location.timestamp=0 → LocationValid 保持 0
}
```

### 5.5 根因总结

```
bcast_powerup → LocationValid = 1        ← 刚设好
    ↓
set_data()    → odid_initUasData() 重置  ← LocationValid 变回 0
              → 没飞控数据，timestamp=0，LocationValid 保持 0
    ↓
transmit_beacon() → LocationValid == 0   ← 构建失败！
```

**`set_data()` 把 `bcast_powerup` 的设置抹掉了**。这是一个代码执行顺序 Bug，对上电即广播模式（无飞控连接时）有决定性影响。

### 5.6 修复

将广播启动条件检查移到 `set_data()` **之后**执行（[RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino)）：

```
修复前：bcast_powerup 检查 → set_data() → 广播     ← 错误
修复后：set_data() → bcast_powerup 检查 → 广播     ← 正确
```

```cpp
// 修复后的代码（第467-484行）
set_data(transport);          // 先填充数据

// 广播启动条件 — 在 set_data 之后，不会被覆盖
if (g.bcast_powerup) {
    if (!UAS_data.LocationValid) {
        UAS_data.Location.Status = ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE;
        UAS_data.LocationValid = 1;    // 现在不会被后续代码覆盖
    }
} else {
    if (last_location_ms == 0) {
        delay(1);
        return;
    }
}

// 广播发射
wifi.transmit_beacon(UAS_data);  // 此时 LocationValid=1，构建成功
```

### 5.7 修复验证

重新编译烧录后，串口输出：

```
BCN: len=87 cnt=3 rate=3.0 payload=29
BCN: OK rate=3.0 valid=1
BCN: len=87 cnt=6 rate=3.0 payload=29
BCN: OK rate=3.0 valid=1
BCN: len=87 cnt=9 rate=3.0 payload=29
BCN: OK rate=3.0 valid=1
```

- **`OK`** — 构建成功
- **`valid=1`** — LocationValid 正确设置
- **`len=87`**, **`payload=29`** — 87 字节完整 Beacon 帧，29 字节 RID 载荷

用嗖嗖FLY 扫描，成功发现 RID 设备。

### 5.8 为什么这个问题之前没暴露

这个 Bug 只在特定条件下触发：

- `BCAST_POWERUP=1`（上电即广播模式）
- **无飞控连接**（没收到 GPS 位置数据）

如果飞控正常工作，`set_data()` 会从飞控收到 `location.timestamp != 0`，从而设置 `LocationValid=1`，Bug 被"掩藏"。Holybro/BlueMark 等产品测试时都有飞控连着，所以从未触发。

### 5.9 清理调试代码

验证通过后，删除 WiFi_TX.cpp 和 RemoteIDModule.ino 中的 `Serial.printf` 调试代码，重新编译烧录最终版。

### 5.10 附：NVS 旧值覆盖问题

修复默认值 `WIFI_BCN_RATE=3` 后还有一个坑：

```
load_defaults() → 加载代码默认值 (WIFI_BCN_RATE=3)
    ↓
nvs_get_*()    → NVS 中已存旧值 (WIFI_BCN_RATE=0) 覆盖默认值
    ↓
最终生效        → WIFI_BCN_RATE=0（还是没效果）
```

解决：在 `Parameters::init()` 中临时插入 `nvs_flash_erase()` 清除 NVS 分区，烧写一次让新默认值生效，然后立即删除临时代码。

## 六、Pixhawk 6C + PX4 飞控联调 — LED 不变绿排查

> 日期：2026-07-17（最后更新：2026-07-17）

### 6.1 背景

连接 Pixhawk 6C（PX4 v1.14.3 飞控）TELEM2 口到 ESP32-C3 的 UART 引脚（GPIO2/GPIO3），飞控工作于模拟飞行模式（无外部 GPS 传感器，输出模拟 GPS 数据）。

### 6.2 现象

- ESP32-C3 能启动，WiFi AP 正常
- Web 管理页面  可看到经纬度数据
- LED 始终保持**红色**，不转为绿色
- Status 显示

### 6.3 飞控侧诊断

#### PX4 v1.14.3 ODID 消息支持情况

通过  (511) 测试 PX4 v1.14.3 支持哪些 ODID 消息：

#### PX4 参数配置

检查 PX4 所有 ODID 相关参数，结果：

- /  /  /  — 存在但无 RemoteID 效果
- PX4 v1.14.3 **没有原生的 ODID 模块参数**（如 、 等不存在）
- — RemoteID module health check arming parameter
- — **初始为 0**，必须手动设置为 1200

### 6.4 调试过程 — 层层深入

#### 第一层：确认 MAVLink 通信建立

在 [mavlink.cpp](RemoteIDModule/mavlink.cpp) 加入串口接收字节数日志：

确认：串口通信正常，ESP32 从 TELEM2 收到飞控数据。

#### 第二层：set_data() 中 Location 数据接收

日志显示两种情况：

#### 第三层：check_parse() 校验结果

### 6.5 定位到四个根因

#### 根因 1：PX4 发送无效的 Location timestamp

PX4 在无真实 GPS 时，（UINT16_MAX 表示未知）。OpenDroneID 库要求 timestamp ≤ 3600 秒。

**修复**（[RemoteIDModule.ino:375](RemoteIDModule/RemoteIDModule.ino#L375)）：

#### 根因 2：check_parse() 无条件校验未接收的消息

 对 Location/System/SelfID/OperatorID 不做 Valid 检查就编码，全零字段导致编码失败。

**修复**（[RemoteIDModule.ino:149](RemoteIDModule/RemoteIDModule.ino#L149)）：

对 System、SelfID、OperatorID 同样加上  守卫。

#### 根因 3：PX4 System 消息 timestamp=0

PX4 的  消息中 ， 中  守卫导致 System 数据从未被填入。

**结论**：保持  守卫不变（timestamp=0 时数据不可靠，不应填入），不改动此逻辑。

#### 根因 4：arm_status_check() 强制要求 PX4 不发的消息

 强制要求 SELF_ID、OP_ID、OP_LOC，PX4 v1.14.3 不支持这些消息。

**修复**（[transport.cpp:61](RemoteIDModule/transport.cpp#L61)）：

### 6.6 PX4 BASIC_ID 主动请求

PX4 默认不发送 ，需通过  (511) 显式请求。

**新增功能**（[mavlink.cpp:307](RemoteIDModule/mavlink.cpp#L307)）：

在  中飞控系统 ID 确认后自动调用一次。

### 6.7 PX4 Remote ID 配置总结

#### PX4 v1.14.3 ODID 消息支持

| 消息 | PX4 支持 | 需MAVLink请求 | 说明                          |
| ---- | -------- | ------------- | ----------------------------- |
|      | ✅       | 否            | 默认 1 Hz                     |
|      | ✅       | 否            | 默认 1 Hz                     |
|      | ✅       | **是**  | PX4 不默认发，需ESP32主动请求 |
|      | ❌       | —            | PX4 不支持                    |
|      | ❌       | —            | —                            |
|      | ❌       | —            | —                            |
|      | ❌       | —            | —                            |
|      | ❌       | —            | —                            |

#### PX4 参数配置清单

| 参数 | 值             | 说明                        |
| ---- | -------------- | --------------------------- |
|      | 115200         | TELEM2 波特率               |
|      | 2              | MAVLink 2                   |
|      | 102 (TELEM2)   | 绑定 MAV 实例1到TELEM2      |
|      | 2 (Onboard)    | 发送 ODID 消息              |
|      | **1200** | ⚠️ 初始为 0，必须手动设置 |
|      | 0              | ESP32 适配下可用 0          |

### 6.8 修复总结

| 文件                                                 | 改动                              | 原因                               |
| ---------------------------------------------------- | --------------------------------- | ---------------------------------- |
| [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino) | 时清零                            | PX4 无GPS发65535，ODID库要求≤3600 |
| [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino) | 仅在  时校验                      | PX4 不发SelfID/OperatorID等        |
| [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino) | 检查移到  之后                    | 修复LocationValid被覆盖Bug         |
| [transport.cpp](RemoteIDModule/transport.cpp)           | SELF_ID/OP_ID/OP_LOC 改为可选检查 | PX4 不支持这些消息                 |
| [mavlink.cpp](RemoteIDModule/mavlink.cpp)               | 新增                              | 向PX4主动请求BASIC_ID              |
| [mavlink.h](RemoteIDModule/mavlink.h)                   | 声明                              | 同上                               |
| [parameters.cpp](RemoteIDModule/parameters.cpp)         | 默认值 0→3                       | 上电即发WiFi Beacon                |

### 6.9 UART RX 引脚更换 — GPIO2 → GPIO10

> 日期：2026-07-20

#### 问题

通过 CH340 串口工具连接到 ESP32 UART 引脚（COM8），发现 ESP32 持续输出 "Waiting for heartbeat"，说明 ESP32 TX 方向正常但**从未收到飞控的 HEARTBEAT**。多次交换 TX/RX 接线后仍然无数据，推断 **GPIO2 硬件可能已损坏**。

#### 验证过程

1. COM8 抓取 ESP32 UART 输出：只看到 "Waiting for heartbeat" 文本，无 MAVLink 帧
2. PX4 USB（COM6）未检测到 ESP32 的 HEARTBEAT（MAV_TYPE_ODID=236）
3. 确认 ESP32 TX（GPIO3）正常输出 MAVLink 帧，但 RX（GPIO2）收不到飞控数据

#### 修复

将 UART RX 引脚从 GPIO2 改为 GPIO10（[board_config.h:39](RemoteIDModule/board_config.h#L39)）：

ESP32-C3 的 GPIO matrix 支持任意 GPIO 映射到 UART，无需改代码其他部分。

### 6.10 波特率不匹配问题排查与修复

> 日期：2026-07-21

#### 问题现象

ESP32 GPIO10 RX 硬件正常，但连接 PX4 TELEM2 后仍显示 "Waiting for heartbeat"，收不到飞控数据。

#### 诊断过程

通过 CH340 串口工具模拟 PX4 发送 MAVLink 心跳和 ODID 消息到 ESP32，确认：

- ESP32 硬件完全正常（GPIO10 RX、GPIO3 TX 都能正常收发）
- ESP32 MAVLink 解析正确（收到模拟数据后 arm_status 变为 GOOD_TO_ARM）
- 问题锁定为**波特率不匹配**

#### 根因

[board_config.h:13](RemoteIDModule/board_config.h#L13) 中 `BAUDRATE` 定义为 **57600**，但 PX4 TELEM2 端口配置为 **115200**。

通过 pymavlink 验证 PX4 TELEM2 (COM9) 在 115200 波特率下正常输出 MAVLink2 数据流（包含 ODID_LOCATION 和 ODID_SYSTEM 消息）。

#### 修复

修改 [board_config.h:13](RemoteIDModule/board_config.h#L13)：

```cpp
// 旧代码
#define BAUDRATE 57600

// 新代码
#define BAUDRATE 115200
```

#### BASIC_ID 缺失问题

连接 PX4 后 arm_status 报错 `[LOC ID SYS]` → 收到 Location/System 但缺 BASIC_ID。

**根因**：PX4 v1.14.3 默认不发送 BASIC_ID（即使通过 `MAV_CMD_SET_MESSAGE_INTERVAL` 请求也无效），因为没有配置 UAS_ID 参数。

**解决方案**：在 ESP32 固件中添加默认 UAS_ID 逻辑（[parameters.cpp:521-537](RemoteIDModule/parameters.cpp#L521-L537)）：

```cpp
// 如果 BasicID 信息不完整，补全默认值
if (!have_basic_id_info()) {
    if (g.ua_type == 0) {
        set_by_name_uint8("UAS_TYPE", 2);     // HELICOPTER_OR_MULTIROTOR
    }
    if (g.id_type == 0) {
        set_by_name_uint8("UAS_ID_TYPE", 1);  // SERIAL_NUMBER
    }
    if (strlen(g.uas_id) == 0) {
        // 用 MAC 地址生成唯一的默认 UAS_ID
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char default_id[21];
        snprintf(default_id, sizeof(default_id), "ESP32-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        set_by_name_string("UAS_ID", default_id);
    }
}
```

#### 验证结果

修复后连接 PX4 TELEM2：

- t=1.9s: arm_status = `FAIL [LOC SYS LOC]`（等待数据）
- t=19.7s: arm_status = **`GOOD []`**（全部校验通过）
- LED 变为**绿色**
- 使用默认 UAS_ID（`ESP32-<MAC地址>`）正常广播

### 6.11 接线确认

最终工作的接线配置：

| PX4 Pixhawk 6C TELEM2 | ESP32-C3 GPIO | 说明                   |
| --------------------- | ------------- | ---------------------- |
| Pin 2 (TX)            | GPIO10 (RX)   | PX4 发送 → ESP32 接收 |
| Pin 3 (RX)            | GPIO3 (TX)    | ESP32 发送 → PX4 接收 |
| Pin 6 (GND)           | GND           | 共地（必须）           |

波特率：**115200**（双方一致）

### 6.12 LED 绿红交替问题 — PX4 发送频率过低导致超时

> 日期：2026-07-22

#### 问题现象

联调成功后，LED 出现**绿色和红色交替**闪烁，而不是稳定绿色。

#### 假设与诊断

初步假设：PX4 发送 ODID 消息频率太低，导致 ESP32 的 `arm_status_check()` 周期性判定数据超时。

**第一步：确认 arm_status 交替**

解析 COM5 上的 MAVLink2 ARM_STATUS 消息（msg_id=12918），观察到规律性切换：

```
t=17.7s  GOOD []
t=20.8s  FAIL [LOC SYS ]     ← 约 3 秒后超时
t=27.0s  GOOD []             ← 收到新数据恢复
t=30.0s  FAIL [LOC SYS ]     ← 又超时
```

**第二步：定位超时阈值**

查看 [transport.cpp](RemoteIDModule/transport.cpp#L45) 的 `arm_status_check()`：

```cpp
const uint32_t max_age_location_ms = 3000;  // 位置数据最大允许年龄：3秒
```

Location 和 System 数据超过 **3 秒**未更新就报错。

**第三步：精确测量 PX4 发送间隔**

在 [mavlink.cpp](RemoteIDModule/mavlink.cpp) 的 LOCATION 接收处加临时调试打印间隔，串口输出：

```
RX Location, interval=0 ms
RX Location, interval=10005 ms
RX Location, interval=10006 ms
```

**确认：PX4 v1.14.3 每 10 秒（0.1Hz）才发送一次 Location/System。**

#### 根因

```
PX4 发送间隔 = 10 秒  >>  ESP32 超时阈值 = 3 秒
    ↓
每帧到达后 3 秒即超时 → 红灯约 7 秒
下一帧到达瞬间 → 绿灯
    ↓
LED 绿红交替
```

#### 方案 A（治本，失败）：请求 PX4 提高频率

扩展 [mavlink.cpp](RemoteIDModule/mavlink.cpp#L307) 的 `request_odid_messages()`，通过
`MAV_CMD_SET_MESSAGE_INTERVAL (511)` 请求 PX4 将 LOCATION/SYSTEM 提高到 2Hz，并带 5 次重发。

**结果：失败。** PX4 v1.14.3 对 ODID 消息的 SET_MESSAGE_INTERVAL 返回 ACK=OK，但**实际不改变发送频率**，仍为固定 10 秒/帧。（此代码保留，未来升级 PX4 或改用 ArduPilot 时可能生效。）

#### 方案 B（治标，采用）：放宽 ESP32 超时阈值

既然 PX4 v1.14.3 的 ODID 流固定为低频且无法调整，将 ESP32 的超时阈值放宽到覆盖
10 秒间隔。修改 [transport.cpp](RemoteIDModule/transport.cpp#L45)：

```cpp
// 旧代码
const uint32_t max_age_location_ms = 3000;   // 3秒

// 新代码
const uint32_t max_age_location_ms = 12000;  // 12秒（覆盖 PX4 的 10 秒间隔）
```

#### 验证结果

修复后监控 35 秒：

```
t=1.4s  FAIL [LOC SYS LOC ]   ← 启动阶段等待首帧
t=6.5s  GOOD []               ← 收到数据转绿
（之后 28.5 秒无任何切换）
```

**LED 稳定绿色，不再交替！**

#### 权衡说明

ASTM F3411 / FAA 标准建议位置数据保持 ~1 秒实时性，但 PX4 v1.14.3 的 ODID 实现
固定 10 秒一帧且不响应提频请求。放宽超时是在此飞控平台上让系统稳定工作的务实选择。
代码注释已标注：若换用能提供高频 ODID 流的飞控（如 ArduPilot），应将阈值调回 3000。

### 6.13 新 PCB 直通线 — UART TX/RX 方向相反导致收不到数据

> 日期：2026-08-13

#### 背景

自研的新 RemoteID PCB 完成，替换 DevKitM-1。新 PCB 的 UART 接口（JST GH 6-pin）硬件定义：

| 连接器 Pin | 信号 | GPIO  |
| ---------- | ---- | ----- |
| Pin 2      | TX   | GPIO3 |
| Pin 3      | RX   | GPIO10 |

使用 **1:1 直通线**接飞控 TELEM2。

#### 问题现象

连接 PX4 飞控后，两个 LED 一直**红色**（收不到 LOCATION/SYSTEM），Web 界面显示 `LOC SYS` 缺失。

#### 根因

飞控 TELEM2 引脚定义（Pixhawk 6C）：

| TELEM2 Pin | 信号     | 方向       |
| ---------- | -------- | ---------- |
| Pin 2      | UART5_TX | 飞控**发送** |
| Pin 3      | UART5_RX | 飞控**接收** |

直通线（Pin2↔Pin2、Pin3↔Pin3）连接后：

- 模块 Pin2 (GPIO3，固件当 TX) ↔ 飞控 Pin2 (TX) → **TX 怼 TX** ❌
- 模块 Pin3 (GPIO10，固件当 RX) ↔ 飞控 Pin3 (RX) → **RX 怼 RX** ❌

模块永远收不到飞控数据。

#### 解决：交换固件端口映射（不改硬件）

硬件连接器已定型（Pin2=GPIO3、Pin3=GPIO10），改用直通线就必须让固件把
GPIO3 当 RX、GPIO10 当 TX。修改 [board_config.h](RemoteIDModule/board_config.h#L38)：

```cpp
// 旧代码（DevKitM-1 杜邦线交叉接）
#define PIN_UART_TX 3
#define PIN_UART_RX 10

// 新代码（新 PCB 直通线）
#define PIN_UART_TX 10   // GPIO10 → Pin3 → 飞控 RX
#define PIN_UART_RX 3    // GPIO3  → Pin2 ← 飞控 TX
```

交换后的直通线信号流：

```
飞控 TELEM2 Pin2 (TX) ──直通──> 模块 Pin2 (GPIO3，现做 RX)  ✓ 收数据
飞控 TELEM2 Pin3 (RX) <──直通── 模块 Pin3 (GPIO10，现做 TX)  ✓ 发数据
```

#### 烧写流程（USB-Serial-JTAG，避坑）

新 PCB 只有板载 USB-Serial-JTAG（COM18，VID:PID=303A:1001），**必须用 Python 版
esptool 烧录**，arduino-cli 内置的 esptool.exe 4.5.1 对 USB-Serial-JTAG 会失败
（报 Write timeout / port doesn't exist）。

**第 1 步：编译并导出 bin**

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3 \
  --libraries "C:/Users/Administrator/Documents/github/ArduRemoteID/libraries" \
  --build-property "build.extra_flags=-DBOARD_ESP32C3_DEV -DESP32" \
  --build-property "build.partitions=partitions" \
  --build-property "upload.maximum_size=2031616" \
  --build-property "build.custom_partitions=C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule/partitions.csv" \
  --export-binaries \
  "C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule"
```

编译产物在 `RemoteIDModule/build/esp32.esp32.esp32c3/`：

| 文件 | 烧录地址 |
| ---- | -------- |
| `RemoteIDModule.ino.bootloader.bin` | 0x0 |
| `RemoteIDModule.ino.partitions.bin`  | 0x8000 |
| `boot_app0.bin`（ESP32 core 目录）    | 0xe000 |
| `RemoteIDModule.ino.bin`              | 0x10000 |

`boot_app0.bin` 不在编译产物里，在 ESP32 core：
`C:/Users/Administrator/AppData/Local/Arduino15/packages/esp32/hardware/esp32/2.0.11/tools/partitions/boot_app0.bin`

**第 2 步：Python esptool 烧录**

```bash
python -m esptool --chip esp32c3 --port COM18 --baud 921600 \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     <build>/RemoteIDModule.ino.bootloader.bin \
  0x8000  <build>/RemoteIDModule.ino.partitions.bin \
  0xe000  <esp32core>/tools/partitions/boot_app0.bin \
  0x10000 <build>/RemoteIDModule.ino.bin
```

#### 验证

重新编译烧录后 LED 转绿，收到 PX4 LOCATION/SYSTEM 数据。✅

#### 经验教训

1. **直通线必须交叉对应**：飞控 Pin2 是 TX，模块 Pin2 就必须是 RX（收），否则 TX 怼 TX。
2. **优先改固件映射而非硬件**：连接器引脚是死的，但固件里 GPIO 的 TX/RX 角色可以交换。
3. **排查顺序**：先确认飞控端口 Pin 定义（TX/RX 方向）→ 对照模块硬件定义 → 判断直通线是否 TX-TX 冲突。
4. **烧录避坑**：USB-Serial-JTAG 烧录用 `python -m esptool`，不要用 arduino-cli 内置 esptool.exe。

## 七、全部修改汇总

| 文件                                                      | 改动                                                | 原因                                            |
| --------------------------------------------------------- | --------------------------------------------------- | ----------------------------------------------- |
| [board_config.h](RemoteIDModule/board_config.h#L19)          | `BOARD_Holybro_RemoteID` → `BOARD_ESP32C3_DEV` | 适配 ESP32-C3-DevKitM-1                         |
| [board_config.h](RemoteIDModule/board_config.h#L13)          | `BAUDRATE` 57600 → **115200**              | 与 PX4 TELEM2 波特率对齐                        |
| [board_config.h](RemoteIDModule/board_config.h#L39)          | `PIN_UART_RX` GPIO2 → **GPIO10**           | GPIO2 硬件损坏，改用 GPIO10                     |
| [WiFi_TX.cpp](RemoteIDModule/WiFi_TX.cpp#L44)                | `max_connection` 1 → 4                           | 允许多设备同时连接 SoftAP                       |
| [parameters.cpp](RemoteIDModule/parameters.cpp#L56)          | `WIFI_BCN_RATE` 默认值 0 → 3                     | 上电即发 WiFi Beacon                            |
| [parameters.cpp](RemoteIDModule/parameters.cpp#L521)         | 新增默认 UAS_ID 初始化逻辑                          | PX4 不发 BASIC_ID 时使用 ESP32 MAC 生成         |
| [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino#L467) | `bcast_powerup` 检查移到 `set_data()` 之后      | 修复 LocationValid 被覆盖的 Bug                 |
| [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino#L375) | `timestamp > 3600` 时清零                         | PX4 无 GPS 时发 65535，ODID 库编码失败          |
| [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino#L149) | `check_parse()` 加 `*Valid` 守卫                | PX4 不发 SelfID/OperatorID 时避免空数据校验     |
| [transport.cpp](RemoteIDModule/transport.cpp#L61)            | SELF_ID/OP_ID/OP_LOC 改为可选检查                   | PX4 v1.14.3 不支持这些消息                      |
| [transport.cpp](RemoteIDModule/transport.cpp#L45)            | `max_age_location_ms` 3000 → **12000**          | PX4 每 10 秒才发一帧，修复 LED 绿红交替         |
| [mavlink.cpp](RemoteIDModule/mavlink.cpp#L307)               | `request_odid_messages()` 扩展为请求 LOC/SYS@2Hz + BASIC_ID@1Hz，带 5 次重发 | 尝试提频（PX4 v1.14.3 不响应，保留备用） |
| [mavlink.h](RemoteIDModule/mavlink.h)                        | 声明 `request_odid_messages()`                    | 同上                                            |
| [board_config.h](RemoteIDModule/board_config.h#L38)          | `PIN_UART_TX` 3→10、`PIN_UART_RX` 10→3          | 新 PCB 直通线接飞控，交换 TX/RX 避免 TX 怼 TX      |

## 八、最终配置

### ESP32 参数

| 参数          | 值                     | 说明                     |
| ------------- | ---------------------- | ------------------------ |
| SSID          | `ArduRemoteID`       | WiFi 热点名称            |
| 密码          | `ArduRemoteID`       | WiFi 热点密码            |
| 管理页面      | `http://192.168.4.1` | Web 管理界面             |
| WIFI_BCN_RATE | 3                      | WiFi Beacon 3 Hz         |
| WIFI_NAN_RATE | 0                      | WiFi NAN 关闭            |
| BT4_RATE      | 1                      | BT4 Legacy 1 Hz          |
| BT5_RATE      | 1                      | BT5 Long Range 1 Hz      |
| BCAST_POWERUP | 1                      | 上电即广播               |
| BAUDRATE      | **115200**       | 串口波特率               |
| UAS_ID        | `ESP32-<MAC>`        | 默认使用 MAC 地址生成    |
| UAS_ID_TYPE   | 1                      | SERIAL_NUMBER            |
| UAS_TYPE      | 2                      | HELICOPTER_OR_MULTIROTOR |

### PX4 参数（必须配置）

| 参数          | 值             | 说明                      |
| ------------- | -------------- | ------------------------- |
| SER_TEL2_BAUD | 115200         | TELEM2 端口波特率         |
| MAV_1_CONFIG  | 102            | 绑定到 TELEM2             |
| MAV_1_MODE    | 2              | Onboard 模式（发送 ODID） |
| MAV_1_RATE    | **1200** | ⚠️ 必须设置（默认为 0） |
| MAV_1_FORWARD | 1              | 启用消息转发              |

### 硬件接线（新 PCB，直通线，固件映射已交换）

| PX4 TELEM2  | 新 PCB 连接器 | ESP32 GPIO   | 信号方向     |
| ----------- | ------------- | ------------ | ------------ |
| Pin 2 (TX)  | Pin 2         | GPIO3 (RX)   | PX4 → ESP32 |
| Pin 3 (RX)  | Pin 3         | GPIO10 (TX)  | ESP32 → PX4 |
| Pin 6 (GND) | Pin 6         | GND          | 共地         |

> 说明：新 PCB 用 1:1 直通线接飞控，固件里 GPIO3 配置为 RX、GPIO10 配置为 TX（见 §6.13）。
> 早期 DevKitM-1 阶段用杜邦线交叉接（飞控 TX→GPIO10、飞控 RX→GPIO3），当时固件为 GPIO3=TX、GPIO10=RX。

### 工作状态指示

| LED 颜色       | arm_status       | 说明                   |
| -------------- | ---------------- | ---------------------- |
| **绿色** | GOOD_TO_ARM (0)  | 所有数据正常，可以解锁 |
| **红色** | PRE_ARM_FAIL (1) | 缺少数据或校验失败     |

arm_status 错误码含义：

- `[LOC]` — 缺少 Location 数据
- `[ID]` — 缺少 BASIC_ID 数据
- `[SYS]` — 缺少 System 数据
- `[]` — 无错误（GOOD）

## 九、调试技巧总结

### 串口监控

ESP32 有两个 MAVLink 通道：

- **Serial (USB / COM5)**: 调试输出 + MAVLink2 通道1（发送心跳、arm_status）
- **Serial1 (GPIO10/GPIO3)**: MAVLink 主通道（与飞控通信）

监控 arm_status：

```python
# 解析 COM5 上的 MAVLink2 ARM_STATUS 消息 (msg_id=12918)
# payload: status(1) + error_string(50)
```

### 模拟飞控测试

通过 CH340 串口工具连接 ESP32 Serial1 模拟 PX4 数据流：

```python
# 发送 HEARTBEAT + BASIC_ID + LOCATION + SYSTEM
# 验证 ESP32 MAVLink 解析和 arm_status 逻辑
```

### 常见问题

1. **"Waiting for heartbeat" 不消失**

   - 检查波特率是否一致
   - 检查 RX/TX 接线（不能接反）
   - 用示波器或逻辑分析仪确认信号电平
2. **arm_status 报 `[ID]`**

   - PX4 v1.14.3 不发 BASIC_ID
   - 使用固件默认 UAS_ID 或通过 Web 界面设置
3. **WiFi Beacon 搜不到**

   - 确认 `WIFI_BCN_RATE` > 0
   - 检查 `bcast_powerup` 逻辑（需在 `set_data()` 之后）
   - 用 WiFi 抓包工具验证 Vendor IE 帧
4. **NVS 参数混乱**

   - 临时在 `Parameters::init()` 中加 `nvs_flash_erase()` 清除
   - 烧录一次后立即删除该行代码

## 十、PX4 v1.14.3 ODID 支持情况

| ODID 消息             | PX4 支持 | 默认发送 | 实测频率 | 说明                     |
| --------------------- | -------- | -------- | -------- | ------------------------ |
| LOCATION (12901)      | ✅       | ✅       | **0.1 Hz（10秒/帧）** | GPS 数据，频率固定不可调 |
| SYSTEM (12904)        | ✅       | ✅       | **0.1 Hz（10秒/帧）** | 操作员位置，频率固定不可调 |
| BASIC_ID (12900)      | ⚠️     | ❌       | —       | 需配置 UAS_ID 参数才发送 |
| SELF_ID (12903)       | ❌       | ❌       | —       | 不支持                   |
| OPERATOR_ID (12905)   | ❌       | ❌       | —       | 不支持                   |
| AUTH (12902)          | ❌       | ❌       | —       | 不支持                   |
| SYSTEM_UPDATE (12919) | ✅       | ✅       | —       | 操作员位置更新           |

PX4 v1.14.3 的 ODID 实现较基础，仅支持核心的 Location/System 消息，足以满足基本的 RemoteID 合规要求。

**重要**：实测 PX4 v1.14.3 的 LOCATION/SYSTEM 发送频率固定为 **0.1 Hz（每 10 秒一帧）**，
且**不响应 `MAV_CMD_SET_MESSAGE_INTERVAL` 提频请求**（返回 ACK=OK 但频率不变）。因此
ESP32 的 `arm_status_check()` 超时阈值必须 ≥ 10 秒（本项目设为 12 秒），否则会出现
LED 绿红交替（详见 §6.12）。

## 十一、成功验证清单

- [X] ESP32 固件编译通过（1659424 bytes, 79%）
- [X] GPIO10 RX 正常接收 MAVLink2 数据
- [X] GPIO3 TX 正常发送心跳和 arm_status
- [X] 波特率对齐（115200）
- [X] 接收 PX4 LOCATION 消息
- [X] 接收 PX4 SYSTEM 消息
- [X] 默认 UAS_ID 生成（基于 MAC 地址）
- [X] arm_status = GOOD_TO_ARM (status=0)
- [X] LED 变绿
- [X] WiFi Beacon 广播（3 Hz）
- [X] BLE 广播（BT4 + BT5）
- [X] 新 PCB 直通线接飞控（固件映射交换：GPIO3=RX、GPIO10=TX）
- [X] 新 PCB LED 转绿（收到 PX4 数据）

**系统完全就绪，可以进行实际飞行测试！**

## 十二、DroneCAN Remote ID 配置（Pixhawk 6C CAN1 联调）

### 12.1 背景

前文 §六~§十一 记录的是 **MAVLink Remote ID**（UART TELEM2，`OPEN_DRONE_ID_*` 消息）。本节记录切换到 **DroneCAN Remote ID**（CAN1，`dronecan.remoteid.*` 消息）的过程。

- PX4 v1.14.3 只支持 MAVLink ODID 流，**DroneCAN Remote ID 需 v1.16.0+**（`src/drivers/uavcan/remoteid.cpp`）
- 飞控升级到 v1.16.2（flight_sw_version=0x011002ff）

### 12.2 DroneCAN 参数状态

实测 Pixhawk 6C 上 DroneCAN 基本参数**出厂即正确**，无需手动设置：

| 参数           | 值        | 说明                             |
| -------------- | --------- | -------------------------------- |
| UAVCAN_ENABLE  | 2         | Sensors Automatic Config（DNA）  |
| UAVCAN_BITRATE | 1000000   | 1 Mbps，与板卡一致               |
| UAVCAN_NODE_ID | 1         | 飞控节点 ID                      |
| SYS_AUTOSTART  | 7001      | 六旋翼，决定 ua_type             |

> 关键：Pixhawk 6C 板级 `rc.board_arch_defaults` 里有 `param set-default -s UAVCAN_ENABLE 2`，所以 UAVCAN_ENABLE 出厂默认就是 2。
> 这带来一个 PX4 参数持久化怪癖：手动把 UAVCAN_ENABLE 设成 2 时，因 user_config(2) == runtime_default(2)，`param_export` 会把它当默认值跳过不导出，旧 SD 卡若残留 UAVCAN_ENABLE=0 会在重启后覆盖回来。正确做法是 `param reset UAVCAN_ENABLE` 或干脆不动它（出厂默认即 2）。

### 12.3 验证 DroneCAN 通信

通过 MAVLink SERIAL_CONTROL 执行 shell 命令 `uavcan status`，输出：

```
Online nodes (Node ID, Health, Mode):
	 123 OK         OPERAT
CAN1 status: RX frames 5723 / TX frames 38244
```

板卡节点 **123 在线**（DNA 动态分配的 ID），CAN1 正常收发。

> 注意：`UAVCAN_NODE_STATUS` MAVLink 消息在 v1.16.2 里**没有对应 stream**（不发送），不能用它判断 DroneCAN 状态，必须用 `uavcan status`。

### 12.4 DroneCAN Remote ID 数据源（v1.16.2）

基本全自动，无需配置：

- **Basic ID**：uas_id = 飞控硬件序列号 GUID，id_type 固定 `SERIAL_NUMBER`，ua_type 由机架 `SYS_AUTOSTART` 决定
- **Location**：自动来自 GPS（无 GPS 时消息照发，值空/未知）
- **SelfID/OperatorID/System**：来自 QGC Remote ID 设置页（MAVLink `OPEN_DRONE_ID_*`，可选）

### 12.5 踩坑记录

1. **模拟模式（SITL/HITL）锁 CAN 参数**：DroneCAN 是真实硬件总线。SITL 无 CAN 硬件（参数不编译）；HITL 绕过真实传感器总线（rcS：`sensors start -h` + `param set GPS_1_CONFIG 0`）。DroneCAN Remote ID 必须真实硬件。
2. **CAN2 空载报错**：CAN2 无设备无终端电阻 → 广播无 ACK → 大量 HW/IO 错误。不影响 CAN1（板卡在 CAN1）。可给 CAN2 加 120Ω 终端电阻消除。
3. **GPS 非必需**：Location 无 GPS 也 1Hz 广播，板卡检查消息新鲜度（12s 超时）而非 GPS 有效性，所以无 GPS 板卡也能转绿（空位置对真实飞行不合规，但验证链路够用）。

## 十三、DroneCAN System 消息时间戳恒定导致红灯（SYS 超时）

### 13.1 现象

DroneCAN 链路已通、Location/System 1Hz 流动、GPS fix_type=3（21–22 颗星），但板卡 LED 仍是**红色双闪**（= ARM_FAIL）。读 MAVLink `OPEN_DRONE_ID_ARM_STATUS`（msg 12918）发现 `status=1 (PRE_ARM_FAIL_GENERIC)`、`error=[SYS]`。

### 13.2 根因

`transport.cpp` 里 System 检查逻辑（上一轮已把 System 改成"收到才查超时"）：

```cpp
if (last_system_ms != 0 && now_ms - last_system_ms > max_age_location_ms) {
    ret += "SYS ";
}
```

能报出 `SYS` 说明板卡**确实收到了 System**（`last_system_ms != 0`），但它**超时了**——即 `last_system_ms` 没被持续刷新。

关键在 `DroneCAN.cpp` 的 `handle_System()` 原实现：

```cpp
if ((last_system_timestamp != pkt.timestamp) || (pkt.timestamp == 0)) {
    last_system_ms = millis();
    last_system_timestamp = pkt.timestamp;
}
```

**按时间戳变化才刷新接收时间。** PX4 的 DroneCAN System 消息虽然 1Hz 持续广播，但消息里的 `timestamp` 字段长期**恒定且非零**（GPS UTC 时间未正常走秒），导致除首帧外不再刷新 `last_system_ms`，12 秒后超时 → `SYS` → 红灯。

> 注：`last_system_timestamp` 是 `uint32_t`（精确），不是 float 精度问题；`Location` 用 float 但时间戳是 uint16（时内秒，0–3599 每秒变化），所以 Location 正常、只有 System 卡住。

### 13.3 修复

`DroneCAN.cpp` `handle_System()`（L640–643）移除时间戳门控，**只要收到 System 消息就刷新**：

```cpp
// PX4 以 1Hz 持续广播 System，但消息里的时间戳字段可能长期不变（GPS UTC 未锁时），
// 若只按时间戳变化刷新会误判为超时。只要收到消息就视为数据新鲜，刷新接收时间。
last_system_ms = millis();
last_system_timestamp = pkt.timestamp;
```

### 13.4 验证

重新编译（自定义 OTA 分区表 `app0=0x1f0000`）+ 烧录后，读 MAVLink：

```
ARM_STATUS (12918): status=0 (GOOD_TO_ARM), error=''   ← 空错误，15 秒内稳定
```

LED 转绿，不再红灯交替。

### 13.5 经验

1. **DroneCAN System 的 timestamp 字段不可靠**：PX4 该字段依赖 GPS UTC 时间，可能长期恒定。判断"数据是否新鲜"应基于**是否持续收到消息**，而非消息内时间戳是否变化。
2. **编译必须带自定义分区表**：默认 ESP32-C3 分区只给 app 1.25MB，ArduRemoteID 固件 ~1.5MB 会报 `text section exceeds available space`。必须加 `--build-property build.partitions=partitions` + `upload.maximum_size=2031616` + `build.custom_partitions=.../partitions.csv`。
3. **MAVLink v2 手动解析**：帧头 10 字节（`0xFD` + len + incompat + compat + seq + sysid + compid + msgid[3]），msgid 取 `buf[7] | buf[8]<<8 | buf[9]<<16`，payload 从 `buf[10]` 起，总长 `10+len+2`。
