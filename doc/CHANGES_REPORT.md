# ArduRemoteID 未提交更改报告

> 生成时间：2026-08-14
> 基线提交：`7033684 CUAV-RID: definition CAN_APP_NODE_NAME`
> 工作区：ESP32-C3 自研 RemoteID 板 + Pixhawk 6C / PX4 v1.16.2 联调

---

## 一、总览

| 项目 | 数量 |
|---|---|
| 已修改文件（`M`） | 36 |
| 新增未跟踪文件/目录（`??`） | 16 |
| 插入行数 | 2300 |
| 删除行数 | 733 |

本次未提交改动包含**两类性质完全不同的变更**：

1. **功能性改动**（约 12 处，影响行为）：围绕 ESP32-C3 自研板联调 + PX4 串口/DroneCAN Remote ID 调通所做的修复与适配。
2. **注释中文化**（约 34 个文件，纯文档性）：将原英文注释逐行翻译为中文、补充协议/算法说明，**不改变任何编译产物或运行行为**。

> 说明：由于「注释中文化」覆盖了绝大多数文件（BLE_TX、CANDriver、DroneCAN、WiFi_TX、check_firmware、efuse、mavlink_secure_command、parameters、romfs、status、webinterface、util 等），下面的功能性清单只列出**实际影响行为的改动**，注释翻译不再逐条展开。

---

## 二、功能性改动明细

### 1. `transport.cpp` — arm 状态检查阈值放宽 + 辅助消息改为可选

**位置**：`arm_status_check()`

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| 位置超时 `max_age_location_ms` | `3000` | `12000` | PX4 v1.14.3 的 ODID 消息流固定 0.1Hz（每 10 秒一帧），且不响应 `SET_MESSAGE_INTERVAL` 提频请求。3 秒阈值导致每帧后 3 秒即超时，LED 绿红交替 |
| SELF_ID 检查 | `last_self_id_ms == 0 \|\| ... 超时` | `last_self_id_ms != 0 && ... 超时` | 改为「仅当飞控确实发过该消息才检查超时」。PX4 默认不发 SELF_ID/OP_ID/System，原先会因「从未收到」而误报 |
| OP_ID 检查 | 同上（`== 0` 即报错） | 同上（改为可选） | 同上 |
| SYS 检查 | `last_system_ms == 0 \|\| ... 超时` | `last_system_ms != 0 && ... 超时` | 同上。PX4 的 System 只在 GCS 配置操作员位置或飞控解锁建立 home 后才发，解锁前不应阻止广播 |
| OP_LOC 检查 | `operator_lat==0 && operator_lon==0` → 报错 | 整段移除（保留空 if 结构） | PX4 可能不发操作员位置，不应因此阻止广播 |

---

### 2. `DroneCAN.cpp` — `handle_System()` 移除时间戳门控

**改前**：
```cpp
if ((last_system_timestamp != pkt.timestamp) || (pkt.timestamp == 0)) {
    last_system_ms = millis();
    last_system_timestamp = pkt.timestamp;
}
```
**改后**：
```cpp
// PX4 以 1Hz 持续广播 System，但消息里的时间戳字段可能长期不变（GPS UTC 未锁时），
// 若只按时间戳变化刷新会误判为超时。只要收到消息就视为数据新鲜，刷新接收时间。
last_system_ms = millis();
last_system_timestamp = pkt.timestamp;
```

**原因**：PX4 以 1Hz 持续广播 System，但 `timestamp` 字段在 GPS UTC 未走秒时长期恒定。原逻辑「时间戳变化才刷新接收时间」导致只刷新一次、12 秒后超时 → `SYS` 错误 → 红灯双闪。这是 DroneCAN 调通后红灯问题的根因，修复后 LED 转绿。

---

### 3. `mavlink.cpp` — 新增 `request_odid_messages()`

**新增函数**：在飞控 sysid 确认后，通过 `MAV_CMD_SET_MESSAGE_INTERVAL`(511) 主动请求：
- `OPEN_DRONE_ID_LOCATION` → 2Hz
- `OPEN_DRONE_ID_SYSTEM` → 2Hz
- `OPEN_DRONE_ID_BASIC_ID` → 1Hz

带 5 次重发（每 3 秒一次），覆盖飞控启动不确定窗口。

**原因**：PX4 默认 LOCATION/SYSTEM 频率太低（约 0.1~0.3Hz），远低于 arm_status_check 超时阈值；BASIC_ID 默认不发，需显式请求。**注意**：实测 PX4 v1.14.3 返回 ACK=OK 但实际不改变频率，代码保留以备换用更高版本 PX4 或 ArduPilot 时生效。

---

### 4. `board_config.h` — 激活板型 + UART 引脚交换

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| 激活板型 | （无显式激活，靠编译参数） | 新增 `#define BOARD_ESP32C3_DEV` | 明确当前目标板为自研 ESP32-C3 板 |
| `PIN_UART_TX`（ESP32C3_DEV） | `3` | `10` | 直通线接飞控 TELEM2 时，飞控 Pin2=TX、Pin3=RX，模块 Pin2=GPIO3、Pin3=GPIO10。原映射 TX=3/RX=2 会「TX 怼 TX」，改映射让 GPIO3 收、GPIO10 发 |
| `PIN_UART_RX`（ESP32C3_DEV） | `2` | `3` | 同上 |
| 未定义板型报错 | `#error "unsupported board"` | 注释掉 | 简化编译，避免误报 |

**原因**：自研新 PCB 的 JST GH 6-pin UART 接口用 1:1 直通线接飞控 TELEM2，直通线必须交叉对应（飞控 TX → 模块 RX）。不改硬件、只交换固件 GPIO 映射即可解决。

---

### 5. `RemoteIDModule.ino` — 3 处修复

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| `check_parse()` 编码检查 | 裸 `{}` 块无条件编码 | `if (UAS_data.XxxValid)` 守卫 | 只对有效数据做编码校验，避免对未填充的结构体编码报假错误 |
| Location 时间戳 | `TimeStamp = location.timestamp` 直接赋值 | 钳位 `if (timestamp <= 3600) ... else 0` | PX4 无 GPS 锁定时发送 `UINT16_MAX=65535`，OpenDroneID 库要求 timestamp ≤ 3600，超出会导致 `encodeLocationMessage()` 失败 |
| `set_data()` 调用位置 | 在 `bcast_powerup` 判断**之后** | 移到判断**之前** | 上电即广播模式下，需先填充数据再做 `LocationValid` 判断，否则默认位置数据不生效 |

---

### 6. `led.cpp` — WS2812 状态机重构 + 修复双 LED bug

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| 状态集 | 仅 2 态：ARM_OK=绿、default=红 | 4 态：INIT=蓝常亮、PFST_FAIL=黄快闪(250ms)、ARM_FAIL=红慢闪(500ms)、ARM_OK=绿常亮 | 与 `led.h` 中已定义的 4 状态枚举对齐，故障状态更可辨 |
| 双 LED 设置 | `setPixelColor(0, 绿)` 后**再次** `setPixelColor(0, 绿)` | `setPixelColor(0, color)` + `setPixelColor(1, color)` | **修复原 bug**：第二行本应设置 LED 2（像素 1），却误写成像素 0，导致第二颗 LED 从未被正确点亮 |
| 刷新周期 | `>= 200` ms | `>= 50` ms | 保证 250ms 快闪可见（200ms 刷新会吞掉快闪相位） |

---

### 7. `WiFi_TX.cpp` — SoftAP 最大连接数

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| `WiFi.softAP(...)` 最大连接数 | `1`（webserver 启用时） | `4` | Web 配置界面需要多个并发连接，1 个连接数不足 |

---

### 8. `BLE_TX.cpp` — 移除不兼容功率等级

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| `dBm_table[]` | 含 `ESP_PWR_LVL_N27`(-27)、`ESP_PWR_LVL_N24`(-24) 共 16 档 | 移除最低两档，剩 14 档（-21 起） | `ESP_PWR_LVL_N27/N24` 仅在 ESP32 Arduino Core 3.x 定义，当前使用的 Core 2.0.11 无此宏，编译失败 |

**影响**：BLE 最低发射功率从 -27dBm 变为 -21dBm，实际使用影响极小。

---

### 9. `parameters.cpp` / `parameters.h` — 默认值调整 + 自动补全 BasicID

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| `BAUDRATE` 默认值 | `57600` | `115200` | 与飞控 TELEM2 常用波特率一致，减少首配步骤 |
| `WIFI_BCN_RATE` 默认值 | `0`（禁用） | `3`（Hz） | 默认开启 WiFi Beacon 广播（Remote ID 主广播方式之一） |
| `wifi_ssid` 默认值 | `""`（空） | `"ArduRemoteID"` | 默认空 SSID 导致 AP 不广播、用户无法发现连接，新烧录固件需可发现 |
| `Parameters::init()` | 无 BasicID 补全逻辑 | 新增：`!have_basic_id_info()` 时自动补 `UAS_TYPE=2`(多旋翼)、`UAS_ID_TYPE=1`(序列号)、`UAS_ID` 用 MAC 生成 `ESP32-XXXXXXXXXXXX` | 无飞控 BASIC_ID 数据时模块也能以合法 ID 广播 |

> ⚠️ 潜在不一致：`parameters.h` 中成员初始化 `uint32_t baudrate = 57600` 仍为 57600，而描述符表默认值已改为 115200。`load_defaults()` 以描述符表为准，故实际生效 115200，但成员初始化器冗余且易误导，建议后续统一。

---

### 10. `mavlink_msgs.h` — include 风格

| 项 | 改前 | 改后 | 原因 |
|---|---|---|---|
| MAVLink 类型头引入 | `#include <mavlink2.h>` | `#include "mavlink2.h"` | 改用双引号查找，优先从工程内库路径解析，规避 Arduino 编译系统头文件搜索路径差异 |

---

## 三、注释中文化（非功能性）

以下 34 个文件的主要改动为**将英文注释翻译为中文 + 补充协议/算法说明**，编译产物不变，仅提升可维护性：

| 类别 | 文件 |
|---|---|
| 广播驱动 | `BLE_TX.cpp/.h`、`WiFi_TX.cpp/.h`、`transmitter.cpp/.h` |
| CAN/DroneCAN | `CANDriver.cpp/.h`、`DroneCAN.cpp/.h` |
| 安全/固件 | `check_firmware.cpp/.h`、`efuse.cpp/.h`、`mavlink_secure_command.cpp` |
| 参数/状态 | `parameters.cpp/.h`、`status.cpp/.h` |
| 传输/MAVLink | `transport.cpp/.h`、`mavlink.cpp/.h`、`mavlink_msgs.h` |
| Web/资源 | `webinterface.cpp/.h`、`romfs.cpp/.h` |
| 工具/其他 | `util.cpp/.h`、`led.cpp/.h`、`version.h`、`options.h` |

翻译中补充了较有价值的技术说明，例如：
- BLE 广播载荷格式（ASTM F3411 帧头 `[长度][0x16][0xFA 0xFF][0x0D][计数器]`）
- CAN 时序计算算法（采样点 87.5% 推导）
- ESP32 CAN 优先级验收过滤器原理（`0x10000000<<3` 只收优先级 ≥16）
- DroneCAN DNA 动态节点 ID 分配流程（Rule C）
- Ed25519 签名验证与公钥 Base64 存储格式（`PUBLIC_KEYV1:` 前缀）
- ROMFS gzip 嵌入与流式读取

---

## 四、未跟踪文件/目录（`??`）

| 路径 | 性质 | 建议 |
|---|---|---|
| `BUILD_NOTES.md`、`Arduino_CLI_Build_Guide.md`、`DEVELOPMENT.md`、`ESP32C3_DEBUG_NOTES.md`、`VSCode_IntelliSense_配置笔记.md` | 开发/编译/调试文档 | 视需要纳入版本库或 .gitignore |
| `portType.md` | 自研板接口引脚定义 | 建议纳入版本库 |
| `libraries/libmav2odid/`、`libraries/libopendroneid/` | 内置的 MAVLink↔ODID 转换库、OpenDroneID 编解码库 | 建议纳入版本库（编译依赖） |
| `RemoteIDModule/dronecan_msgs.h` | DroneCAN DSDL 生成消息汇总头 | 生成物，建议纳入或标记自动生成 |
| `RemoteIDModule/build/` | arduino-cli 编译产物 | **应加入 .gitignore** |
| `.vscode/`、`.embeddedskills/` | 编辑器/工具链配置 | 视团队约定决定是否纳入 |
| `3600` | 空文件（0 字节） | 疑似误生成（命令重定向残留），建议删除 |

---

## 五、小结与建议

**本次联调核心修复链**（DroneCAN 红灯 → 绿灯）：
1. `transport.cpp`：位置超时 3s→12s，辅助消息（SELF_ID/OP_ID/SYS）改为可选
2. `DroneCAN.cpp`：`handle_System()` 移除时间戳门控（根因修复）
3. `mavlink.cpp`：主动请求 ODID 消息提频（预留，PX4 v1.14.3 未生效）
4. `board_config.h`：UART 引脚交换适配直通线
5. `RemoteIDModule.ino`：Location 时间戳钳位，避免无 GPS 锁时编码失败

**建议的后续清理项**：
- 统一 `baudrate` 默认值（成员初始化器 57600 与描述符表 115200 不一致）
- 删除空文件 `3600`、将 `RemoteIDModule/build/` 加入 `.gitignore`
- 确认 `libraries/libmav2odid`、`libopendroneid` 是否应作为子模块或直接纳入版本库
