# ArduRemoteID 开发文档

## 一、项目概述

ArduRemoteID 是一个运行在 ESP32 系列 MCU 上的 **OpenDroneID 远程识别（Remote ID）模块**。它从飞控（通过 MAVLink 或 DroneCAN 协议）接收无人机状态数据，编码为 ASTM F3411 / ASD-STAN prEN 4709-002 标准的 OpenDroneID 格式，再通过 **WiFi（NAN/Beacon）** 和 **BLE（BT4/BT5）** 广播出去，供地面的远程 ID 接收器（如手机 App）接收。

**核心能力**:

| 功能      | 描述                                          |
| --------- | --------------------------------------------- |
| 数据接收  | MAVLink 串口 或 DroneCAN 总线                 |
| WiFi 广播 | NAN Action 帧 / Beacon 信标帧                 |
| BLE 广播  | BT4 Legacy (兼容广) / BT5 Long Range (距离远) |
| Web 管理  | WiFi AP + HTTP 服务器（参数配置、OTA 升级）   |
| OTA 升级  | 带 Ed25519 签名的固件空中升级                 |
| 安全      | eFuse 熔丝锁定、签名验证、参数保护            |

**支持 12 种硬件板型**（见 `board_config.h`）：ESP32-S3/C3 开发板、BlueMark DB200/DB110/DB202/DB210PRO/DB203、mRo RID、Holybro RemoteID、CUAV C-RID 等。

---

## 二、目录结构

```
ArduRemoteID/
├── RemoteIDModule/          # 主固件代码（Arduino 项目目录）
│   ├── RemoteIDModule.ino   # 主程序入口 setup() / loop()
│   │
│   ├── 传输层（数据接收）
│   │   ├── transport.h/cpp       # 传输层基类，持有共享 RID 数据
│   │   ├── mavlink.h/cpp         # MAVLink 串口传输
│   │   ├── mavlink_msgs.h        # MAVLink 消息配置（通道、缓冲区）
│   │   ├── mavlink_secure_command.cpp  # MAVLink 安全命令处理
│   │   ├── DroneCAN.h/cpp        # DroneCAN 总线传输
│   │   ├── dronecan_msgs.h       # DroneCAN DSDL 消息汇总
│   │   ├── CANDriver.h/cpp       # ESP32 TWAI (CAN) 底层驱动
│   │   └── mavlink2.h            # MAVLink 库版本选择
│   │
│   ├── 广播层（数据发射）
│   │   ├── transmitter.h/cpp     # 发射器基类（随机 MAC 生成）
│   │   ├── WiFi_TX.h/cpp         # WiFi NAN + Beacon 广播
│   │   └── BLE_TX.h/cpp          # BLE BT4 Legacy + BT5 Long Range 广播
│   │
│   ├── 配置与管理
│   │   ├── parameters.h/cpp      # 参数系统（NVS 存储、MAVLink/DroneCAN 协议接口）
│   │   ├── options.h             # 编译时功能开关（启用/禁用 MAVLink/DroneCAN）
│   │   ├── board_config.h        # 12 种板型硬件引脚定义
│   │   └── version.h             # 固件版本号
│   │
│   ├── Web 与 OTA
│   │   ├── webinterface.h/cpp    # Web 管理界面（参数查看、OTA 上传）
│   │   ├── check_firmware.h/cpp  # OTA 固件 Ed25519 签名验证
│   │   ├── romfs.h/cpp           # 只读文件系统（Web 静态资源嵌入 Flash）
│   │   └── romfs_files.h         # 自动生成的嵌入文件数据（HTML/JS/CSS）
│   │
│   ├── 状态与指示
│   │   ├── status.h/cpp          # 状态 JSON 生成（Web 界面轮询）
│   │   └── led.h/cpp             # LED 驱动（WS2812 / GPIO）
│   │
│   ├── 工具与安全
│   │   ├── util.h/cpp            # CRC64 + Base64 编解码
│   │   └── efuse.h/cpp           # eFuse 熔丝配置（一次性安全锁定）
│   │
│   └── 第三方库
│       ├── monocypher.h/cpp      # Ed25519 签名验证库
│       ├── tinf.h / tinfgzip.cpp / tinflate.cpp  # gzip 解压库
│       └── git-version.h         # 编译时自动生成的 Git commit hash
│
├── libraries/
│   ├── libmav2odid/             # MAVLink ↔ OpenDroneID 转换库
│   └── libopendroneid/          # OpenDroneID 协议编解码库
│
└── scripts/                     # 编译和构建脚本
```

---

## 三、系统架构

### 3.1 数据流

```
飞控 (ArduPilot/PX4)
    │
    ├── MAVLink 串口 ──→ [mavlink.cpp] ──┐
    │                                     ├──→ [transport.cpp 共享 RID 数据]
    └── DroneCAN 总线 ─→ [DroneCAN.cpp] ─┘              │
                                                         │
                    ┌────────────────────────────────────┘
                    ▼
            [RemoteIDModule.ino]
              set_data() 填充 ODID_UAS_Data
              check_parse() 验证数据合法性
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
   [WiFi_TX]   [BLE_TX]    [status.cpp]
   NAN/Beacon  BT4/BT5     JSON 状态
```

**关键设计**:

- **传输层共享数据**：`Transport` 基类中所有 RID 数据字段都是 `static` 的。无论数据从 MAVLink 还是 DroneCAN 传来，写入同一份共享数据。最新收到的数据覆盖旧数据。
- **广播层独立**：WiFi 和 BLE 发射器各自维护发送计数器和状态，互不干扰。
- **主循环调度**：`loop()` 中以固定频率轮询通信通道，按配置的速率间隔触发各广播通道。

### 3.2 类继承关系

```
Transport (传输层基类，持有共享 RID 数据)
    ├── MAVLinkSerial (MAVLink 串口)
    └── DroneCAN (DroneCAN + CAN 驱动)

Transmitter (发射器基类，随机 MAC)
    ├── WiFi_TX (WiFi NAN + Beacon)
    └── BLE_TX (BT4 Legacy + BT5 Long Range)

Parameters (参数管理，单例 g)
CheckFirmware (OTA 签名验证)
WebInterface (Web 服务器)
ROMFS / ROMFS_Stream (嵌入式文件系统)
Led (LED 指示灯)
```

---

## 四、模块详解

### 4.1 主程序 [RemoteIDModule.ino](RemoteIDModule/RemoteIDModule.ino)

**setup()** — 一次性初始化：

1. 禁用欠压检测 → 2. 加载参数（NVS）→ 3. 初始化 WiFi（用于 Web 界面）→ 4. 初始化串口（USB + UART）→ 5. 初始化 ODID 数据结构 → 6. 初始化 MAVLink/DroneCAN 通道 → 7. 验证当前运行的 OTA 固件 → 8. 配置 CAN 引脚（使能/终端电阻）→ 9. 标记 OTA 分区有效

**loop()** — 循环执行（~1ms 周期）：

1. 轮询 MAVLink / DroneCAN 接收数据
2. 数据超时检测（>5s 无数据 → 标记 REMOTE_ID_SYSTEM_FAILURE）
3. Web 界面更新
4. 填充 UAS_data → 数据验证
5. 按配置速率触发 WiFi_NA N → WiFi_Beacon → BT5 → BT4 广播

**BT4 轮流发送机制**：BT4 传统广播每次只能发一条 ODID 消息（31 字节限制），因此需要轮流发送 6-7 种消息。循环间隔 = `(1000 / states) / rate`，例如 rate=300、6 种消息时，每 0.56ms 发一条，完整包耗时 3.3ms。

### 4.2 传输层基类 [transport.h/cpp](RemoteIDModule/transport.h)

**共享的 RID 数据字段**（全部 static，跨协议共享）：

| 字段            | 类型                                    | 说明                     |
| --------------- | --------------------------------------- | ------------------------ |
| `location`    | `mavlink_open_drone_id_location_t`    | 位置/速度/航向           |
| `basic_id`    | `mavlink_open_drone_id_basic_id_t`    | 无人机基本标识           |
| `self_id`     | `mavlink_open_drone_id_self_id_t`     | 自我标识（描述文本）     |
| `system`      | `mavlink_open_drone_id_system_t`      | 系统信息（操作员位置等） |
| `operator_id` | `mavlink_open_drone_id_operator_id_t` | 操作员标识               |

**arm_status_check()** — 解锁条件检查：

- 位置数据 < 3 秒超时
- 其他数据 < 22 秒超时
- 必须有 BasicID / SelfID / OperatorID / System
- 飞机和操作员位置不能为 (0,0)
- `OPTIONS_FORCE_ARM_OK` 选项可跳过所有检查

**会话密钥生成**：由 MAC 地址 + micros() + 随机数 → CRC64 → 取 8 字节，用于安全命令的防重放攻击。

**Ed25519 签名验证**：遍历所有已配置公钥，使用 Monocypher 增量验证 API。签名数据 = `sequence(4B) + operation(4B) + data + session_key(8B)`。

### 4.3 MAVLink 传输 [mavlink.h/cpp](RemoteIDModule/mavlink.h)

通过串口（UART 或 USB）与飞控通信。支持双通道：`Serial1`（连接飞控，COMM_0）和 `Serial`（USB 调试，COMM_1）。

**接收消息处理**（`process_packet()`）：

| MAVLink 消息                    | 处理逻辑                          |
| ------------------------------- | --------------------------------- |
| `HEARTBEAT`                   | 自动获取飞控 sysid/compid         |
| `OPEN_DRONE_ID_LOCATION`      | 写入共享 location                 |
| `OPEN_DRONE_ID_BASIC_ID`      | 写入共享 basic_id，可持久化到 NVS |
| `OPEN_DRONE_ID_SYSTEM`        | 写入共享 system                   |
| `OPEN_DRONE_ID_SELF_ID`       | 写入共享 self_id                  |
| `OPEN_DRONE_ID_OPERATOR_ID`   | 写入共享 operator_id              |
| `OPEN_DRONE_ID_ARM_STATUS`    | 发送解锁状态响应                  |
| `SECURE_COMMAND`              | 安全命令处理（密钥管理）          |
| `PARAM_REQUEST_LIST/READ/SET` | 参数协议                          |

**发送消息**：心跳（1Hz）、解锁状态（1Hz）、STATUSTEXT（调试信息）。

### 4.4 DroneCAN 传输 [DroneCAN.h/cpp](RemoteIDModule/DroneCAN.h)

通过 CAN 总线与飞控通信。使用 **libcanard** 轻量级 UAVCAN 协议栈，底层驱动为 ESP32 TWAI（CAN 2.0B）。

**核心组件**：

- `CanardInstance` — libcanard 协议栈上下文
- `CANDriver` — ESP32 TWAI 外设封装
- `canard_memory_pool[1024]` — 4KB Canard 内存池

**接收消息处理**：

| DroneCAN 消息                                  | 端口 ID | 处理                |
| ---------------------------------------------- | ------- | ------------------- |
| `dronecan.remoteid.BasicID`                  | 32410   | → 共享 basic_id    |
| `dronecan.remoteid.Location`                 | 32411   | → 共享 location    |
| `dronecan.remoteid.SelfID`                   | 32412   | → 共享 self_id     |
| `dronecan.remoteid.System`                   | 32413   | → 共享 system      |
| `dronecan.remoteid.OperatorID`               | 32414   | → 共享 operator_id |
| `dronecan.remoteid.ArmStatus`                | 响应    | ← 解锁状态         |
| `dronecan.remoteid.SecureCommand`            | 32415   | 安全命令            |
| `uavcan.protocol.param.GetSet`               | —      | 参数读写            |
| `uavcan.protocol.GetNodeInfo`                | —      | 节点信息查询        |
| `uavcan.protocol.dynamic_node_id.Allocation` | —      | DNA 节点 ID 分配    |

**动态节点 ID 分配（DNA）**：如果 `g.can_node == 0`（自动分配），模块会通过 UAVCAN DNA 协议向总线申请节点 ID。先用 ESP32 eFuse MAC 的 CRC32 作为唯一 ID，若发生冲突则附加随机数重新申请。

**发送**：NodeStatus（1Hz，包含健康/模式/运行时间）、ArmStatus（1Hz）。

### 4.5 CAN 底层驱动 [CANDriver.h/cpp](RemoteIDModule/CANDriver.h)

封装 ESP32 TWAI 外设：

- 波特率：**1 Mbps**
- 接收队列：50 帧（防止溢出）
- 发送队列：5 帧
- 接收过滤器：单过滤器模式，仅接收 DroneCAN 相关帧
- 错误恢复：BUS_OFF 状态每 2 秒尝试恢复；STOPPED 状态自动重启

`computeTimings()` 实现了完整的 CAN 时序参数计算（prescaler/BS1/BS2/SJW），目标是采样点接近 87.5%。

### 4.6 WiFi 广播 [WiFi_TX.h/cpp](RemoteIDModule/WiFi_TX.h)

**NAN 模式**：

- 使用 WiFi NAN（WiFi Aware）协议
- 通过 Action 帧发送 ODID 数据
- 需要接收端支持 NAN
- 每次发送前随机更换 MAC 地址（隐私保护）

**Beacon 模式**：

- 将 ODID 数据编码到 WiFi 信标帧的 Vendor Specific IE 中
- SSID 格式包含部分 ODID 信息
- 兼容性好，任何 WiFi 设备都可接收
- 需要初始化 WiFi AP（仅发送，不需要客户端连接）

**发射功率转换**：`dBm_to_tx_power()` 将 dBm 值映射到 ESP32 硬件功率等级（0-84），支持 2-20 dBm 范围。

### 4.7 BLE 广播 [BLE_TX.h/cpp](RemoteIDModule/BLE_TX.h)

**BT4 Legacy 模式**（`transmit_legacy()`）：

- 使用传统 BLE 广播（ADV_NONCONN_IND）
- 每次只能发送 31 字节载荷
- **轮流发送**：BasicID → BasicID2 → Location → SelfID → System → OperatorID → (循环)
- 每条消息的广播载荷格式：`[len][AD Type=0x16][Service UUID=0xFFFA][ODID Msg Type][Counter][ODID Payload]`
- 兼容所有手机（包括老旧设备）

**BT5 Long Range 模式**（`transmit_longrange()`）：

- 使用 BLE 5.0 扩展广播 + Coded PHY（S8 编码方案）
- 单次发送所有 ODID 消息（最多 250 字节）
- 传输距离约为 BT4 的 4 倍
- 需要接收端支持 BLE 5（iPhone 11+ / Android 8+）

**消息计数器**：每条消息类型有独立的 8 位计数器，用于接收端检测丢包。

### 4.8 参数系统 [parameters.h/cpp](RemoteIDModule/parameters.h)

**参数表 `params[]`** 定义了所有可配置参数。按功能分为 6 组：

| 分组      | 参数                 | 类型   | 说明                               |
| --------- | -------------------- | ------ | ---------------------------------- |
| 系统参数  | `LOCK_LEVEL`       | INT8   | 0=未锁定, 1=参数锁定, 2=eFuse 锁定 |
|           | `CAN_NODE`         | UINT8  | DroneCAN 节点 ID（0=自动 DNA）     |
|           | `BCAST_PWRUP`      | UINT8  | 上电即广播（0=等 GPS 数据）        |
|           | `FORCE_ARM`        | UINT8  | 强制解锁（1=跳过数据检查）         |
|           | `BAUDRATE`         | UINT32 | MAVLink 串口波特率                 |
| BasicID   | `UAS_TYPE`         | UINT8  | 无人机类型（多旋翼/固定翼等）      |
|           | `UAS_ID_TYPE`      | UINT8  | ID 类型（序列号/注册号等）         |
|           | `UAS_ID`           | CHAR20 | UAS 标识字符串                     |
|           | `UAS_TYPE_2`       | UINT8  | 第二 BasicID 类型                  |
|           | `UAS_ID_TYPE_2`    | UINT8  | 第二 BasicID ID 类型               |
|           | `UAS_ID_2`         | CHAR20 | 第二 BasicID 标识                  |
| WiFi/广播 | `WIFI_NAN_RATE`    | FLOAT  | NAN 广播速率（Hz, 0=关闭）         |
|           | `WIFI_BEACON_RATE` | FLOAT  | Beacon 广播速率                    |
|           | `WIFI_POWER`       | FLOAT  | WiFi 功率（dBm）                   |
|           | `BT4_RATE`         | FLOAT  | BT4 广播速率                       |
|           | `BT4_POWER`        | FLOAT  | BT4 功率                           |
|           | `BT5_RATE`         | FLOAT  | BT5 广播速率                       |
|           | `BT5_POWER`        | FLOAT  | BT5 功率                           |
| Web 界面  | `WEBSERVER`        | UINT8  | Web 服务器使能                     |
|           | `WIFI_SSID`        | CHAR20 | WiFi AP 名称                       |
|           | `WIFI_PASSWORD`    | CHAR20 | WiFi AP 密码                       |
|           | `WIFI_CHANNEL`     | UINT8  | WiFi 信道                          |
| 安全      | `PUBLIC_KEY1-5`    | CHAR64 | Ed25519 公钥（Base64）             |
| MAVLink   | `MAVLINK_SYSID`    | UINT8  | MAVLink 系统 ID                    |

**存储机制**：参数存储在 ESP32 NVS（Non-Volatile Storage），修改后立即写入。`TO_DEFAULTS` 特殊参数写入 1 可恢复出厂设置。

**远程修改**：支持通过 MAVLink `PARAM_SET` 和 DroneCAN `GetSet` 协议远程修改参数。

### 4.9 Web 管理界面 [webinterface.h/cpp](RemoteIDModule/webinterface.h)

WiFi AP 模式 + HTTP 服务器：

- **ROMFS_Handler**：从 Flash 中的 romfs 提供静态文件（HTML/JS/CSS，gzip 压缩）
- **AJAX_Handler**：`/ajax/status.json` 返回实时 RID 状态 JSON（调用 `status_json()`）
- **OTA 端点**：`/update` 接受固件上传

**OTA 更新流程**：

1. 客户端通过 HTTP POST 上传固件
2. 缓存前 16 字节（lead_bytes）用于签名验证
3. 数据分块写入 OTA Flash 分区
4. 上传完成后写入一个扇区+1 字节的 0xFF 填充（强制刷新写缓冲）
5. 调用 `CheckFirmware::check_OTA_next()` 验证 Ed25519 签名
6. 验证通过 → 提交更新 + 重启；失败 → 拒绝更新

### 4.10 OTA 签名验证 [check_firmware.h/cpp](RemoteIDModule/check_firmware.h)

**安全策略**（`check_OTA_next()` 中）：

1. 无公钥时接受任何固件（方便开发）
2. `lock_level == -1` 时跳过所有验证（调试模式）
3. 板 ID (`board_id`) 不为零且不匹配时拒绝（防止刷错固件）
4. 使用所有已配置 Ed25519 公钥逐个尝试验证签名

**验证流程**（`check_OTA_partition()`）：

1. 将 Flash 分区 mmap 到内存
2. 搜索反转魔数 `APP_DESCRIPTOR_REV` → 定位应用描述符
3. 验证描述符中 `image_size` 与实际偏移一致
4. 使用 Monocypher Ed25519 增量验证 API：`Init(Sig, PubKey) → Update(lead_bytes) → Update(Flash) → Final()`

### 4.11 状态 JSON [status.h/cpp](RemoteIDModule/status.h)

`status_json()` 生成包含所有 RID 字段的 JSON 字符串，供 Web 界面 AJAX 轮询显示：

- 版本信息（固件版本、Git hash、板型名称）
- 各传输协议状态（MAVLink/DroneCAN 的心跳和数据更新状态）
- ODID 消息内容（BasicID/Location/SelfID/System/OperatorID）
- 广播状态（各通道的广播速率和功率）

### 4.12 LED 指示 [led.h/cpp](RemoteIDModule/led.h)

支持两种 LED 类型：

- **WS2812 RGB LED**：`WS2812_LED_PIN` 定义（七彩灯带）
- **GPIO LED**：`PIN_STATUS_LED` 定义（普通亮灭）

**状态指示**：

| 状态     | LED 表现                        |
| -------- | ------------------------------- |
| INIT     | 蓝色闪烁（初始化中）            |
| ARM_OK   | 绿色常亮（自检通过 + 数据正常） |
| ARM_FAIL | 红色闪烁（自检失败或数据异常）  |

### 4.13 工具函数 [util.h/cpp](RemoteIDModule/util.h)

- **CRC64**：多项式 `0x42F0E1EBA9EA3693`（ECMA-182），用于固件完整性校验和会话密钥生成
- **Base64 解码**：将 Base64 字符串解码为 32 字节 Ed25519 公钥
- **Base64 编码**：将 32 字节公钥编码为 Base64 字符串存储

### 4.14 eFuse 安全 [efuse.h/cpp](RemoteIDModule/efuse.h)

ESP32 eFuse 熔丝位是一次性可编程（OTP）的硬件安全机制。当 `lock_level >= 2` 时：

1. 禁用 JTAG 调试接口
2. 禁用 UART 下载模式
3. 设置 eFuse 写保护

**注意**：熔丝烧录后不可逆，仅在生产发布时使用。

### 4.15 ROMFS 嵌入式文件系统 [romfs.h/cpp](RemoteIDModule/romfs.h)

将 Web 界面静态文件（HTML/CSS/JS/图片）以 gzip 压缩格式嵌入到固件 Flash 中。

- `romfs_files.h` 由脚本自动生成，包含压缩后的文件数据
- `ROMFS_Stream` 继承 Arduino `Stream`，提供流式读取
- `find_string()` 使用 uzlib 解压 gzip 文件（用于加载嵌入的文本数据如公钥）

---

## 五、编译配置

### 5.1 板型选择

编辑 [board_config.h](RemoteIDModule/board_config.h) 开头的 `#define`：

```cpp
#define BOARD_Holybro_RemoteID     // 当前使用的板型
```

可选板型：`BOARD_ESP32S3_DEV`、`BOARD_ESP32C3_DEV`、`BOARD_BLUEMARK_DB200`、`BOARD_BLUEMARK_DB110`、`BOARD_JW_TBD`、`BOARD_MRO_RID`、`BOARD_JWRID_ESP32S3`、`BOARD_BLUEMARK_DB202`、`BOARD_BLUEMARK_DB210`、`BOARD_BLUEMARK_DB203`、`BOARD_Holybro_RemoteID`、`BOARD_CUAV_RID`

### 5.2 通信协议开关

[options.h](RemoteIDModule/options.h)：

```cpp
// DroneCAN：由板级配置决定（有 CAN 引脚则启用）
#define AP_DRONECAN_ENABLED defined(PIN_CAN_TX) && defined(PIN_CAN_RX)

// MAVLink：始终启用
#define AP_MAVLINK_ENABLED 1
```

### 5.3 Arduino CLI 编译

```bash
# 安装 ESP32 平台
arduino-cli core install esp32:esp32

# 编译（以 Holybro RemoteID 为例）
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  --build-property "build.partitions=min_spiffs" \
  RemoteIDModule/
```

### 5.4 添加新板型

1. 在 [board_config.h](RemoteIDModule/board_config.h) 中添加 `#elif defined(BOARD_xxx)` 块
2. 定义必需引脚：`PIN_CAN_TX/RX`（DroneCAN）或 `PIN_UART_TX/RX`（MAVLink）
3. 定义 LED 引脚：`WS2812_LED_PIN` 或 `PIN_STATUS_LED`
4. 可选：`PIN_CAN_EN`、`PIN_CAN_nSILENT`、`PIN_CAN_TERM`、`BUZZER_PIN`、`CAN_APP_NODE_NAME`

---

## 六、运行时参数配置

参数通过 MAVLink 地面站（Mission Planner / QGroundControl）或 DroneCAN 配置工具修改。

### 6.1 broadcast_powerup 广播模式

| 值    | 模式       | 行为                                                                        |
| ----- | ---------- | --------------------------------------------------------------------------- |
| `0` | 正常模式   | 等待收到 GPS 位置后再开始广播                                               |
| `1` | 上电即广播 | 一旦上电立即广播，位置用默认值（全零），状态标记为 REMOTE_ID_SYSTEM_FAILURE |

### 6.2 广播速率配置

所有 `*_RATE` 参数单位是 **Hz**（每秒广播次数）。设置为 `0` 则关闭该通道。

**推荐配置**：

| 场景             | WIFI_NAN_RATE | WIFI_BEACON_RATE | BT4_RATE | BT5_RATE |
| ---------------- | ------------- | ---------------- | -------- | -------- |
| 仅 BT5 远距离    | 0             | 0                | 0        | 3        |
| 仅 BT4 兼容      | 0             | 0                | 3        | 0        |
| BT4 + BT5 双发   | 0             | 0                | 2        | 2        |
| WiFi + BT5       | 3             | 0                | 0        | 3        |
| 全开（功耗最高） | 2             | 2                | 2        | 2        |

### 6.3 安全锁定

| LOCK_LEVEL | 状态       | 效果                                                    |
| ---------- | ---------- | ------------------------------------------------------- |
| `0`      | 未锁定     | 参数可自由修改                                          |
| `1`      | 参数锁定   | 参数不可修改（`PARAM_SET` 被拒绝）                    |
| `2`      | eFuse 锁定 | 参数锁定 + 禁用 JTAG + 禁用下载模式（**不可逆**） |
| `-1`     | 调试模式   | 跳过所有安全验证（仅开发用）                            |

### 6.4 选项位掩码（OPTIONS）

通过参数 `OPTIONS` 按位组合：

| 位    | 宏                                           | 效果                              |
| ----- | -------------------------------------------- | --------------------------------- |
| bit 0 | `OPTIONS_FORCE_ARM_OK`                     | 强制解锁（跳过所有数据验证）      |
| bit 1 | `OPTIONS_DONT_SAVE_BASIC_ID_TO_PARAMETERS` | 不自动保存 BasicID 到 NVS         |
| bit 2 | `OPTIONS_PRINT_RID_MAVLINK`                | 串口打印收到的 RID 消息（调试用） |

---

## 七、安全机制

### 7.1 Ed25519 签名验证

用于两部分：

1. **OTA 固件验证**：固件映像末尾嵌入 Ed25519 签名的 `app_descriptor_t`。模块会遍历所有已配置公钥验证签名。
2. **安全命令**：MAVLink/DroneCAN 的安全命令（修改公钥列表、获取会话密钥等）需携带 Ed25519 签名。

### 7.2 公钥管理

- 最多存储 **5 个** Ed25519 公钥（每个 32 字节）
- 存储格式：NVS 中以 Base64 编码存储
- 设置方式：通过安全命令（需要现有公钥签名验证）
- 初始加载：首次启动时从 ROMFS 中加载内嵌公钥（`public_key.dat`）

### 7.3 防重放攻击

安全命令签名中包含 `session_key`。会话密钥由以下数据 CRC64 生成：

```
MAC地址(8B) + micros()(4B) + random(4B)
```

每次签名验证时，请求者需先通过 `GET_SESSION_KEY` 获取当前会话密钥，再将其包含在命令签名中。

### 7.4 eFuse 硬件安全

`LOCK_LEVEL=2` 时：

- 禁用 ESP32 JTAG 调试接口（防止固件逆向）
- 禁用 ROM 下载模式（防止强制刷写未签名固件）
- 一次性操作，不可逆

---

## 八、开发指南

### 8.1 添加新的广播通道

1. 创建新类继承 `Transmitter`（如 `class LoRa_TX : public Transmitter`）
2. 实现 `init()` 和 `transmit()` 方法
3. 在 `parameters.h` 中添加对应速率/功率参数
4. 在 `RemoteIDModule.ino` 的 `loop()` 中添加调度逻辑
5. 在 `board_config.h` 中添加对应引脚定义

### 8.2 添加新的通信协议

1. 创建新类继承 `Transport`
2. 实现 `init()` 和 `update()` 方法
3. 在 `update()` 中解析协议数据，写入共享 RID 字段（`location`、`basic_id` 等静态成员）
4. 设置对应的时间戳 `last_location_ms`、`last_system_ms` 等

### 8.3 添加新的 RID 消息类型

1. 在 `transport.h` 中添加共享数据字段
2. 在 `mavlink.cpp` 中添加对应 MAVLink 消息的解析处理
3. 在 `DroneCAN.cpp` 中添加对应的 Canard 回调处理
4. 在 `RemoteIDModule.ino` 的 `set_data()` 中填充 `UAS_data` 对应字段
5. 在 `check_parse()` 中添加编码验证
6. 在 `BLE_TX.cpp` / `WiFi_TX.cpp` 中添加广播编码
7. 在 `status.cpp` 中添加 JSON 状态输出

### 8.4 调试技巧

- 串口波特率 115200，输出大量 `Serial.printf()` 调试信息
- `OPTIONS_PRINT_RID_MAVLINK` 可在串口打印收到的 RID 消息
- `lock_level = -1` 可跳过所有签名验证
- CAN 通信调试：`can_printf()` 通过 CAN LogMessage 发送日志
- Web 界面：访问 `http://<ESP32_IP>/ajax/status.json` 查看实时状态

### 8.5 IntelliSense / IDE 配置

项目使用预编译器宏（如 `#if AP_DRONECAN_ENABLED`）进行条件编译。VS Code 需在 `.vscode/c_cpp_properties.json` 中配置 `defines` 以正确解析所有代码路径。

---

## 九、关键常量和限制

| 项目                    | 值   | 说明                  |
| ----------------------- | ---- | --------------------- |
| MAX_PUBLIC_KEYS         | 5    | 最大 Ed25519 公钥数量 |
| PUBLIC_KEY_LEN          | 32   | 每个公钥字节数        |
| PARAM_NAME_MAX_LEN      | 16   | 参数名称最大长度      |
| CAN_POOL_SIZE           | 4096 | Canard 内存池（字节） |
| ODID_MSG_COUNTER_AMOUNT | 20   | BT4 消息计数器种类    |
| FW_VERSION              | 1.14 | 当前固件版本          |
| APP_DESCRIPTOR_REV      | —   | 反转魔数（8字节）     |

---

## 十、依赖库

| 库                 | 用途                 | 来源                |
| ------------------ | -------------------- | ------------------- |
| libopendroneid     | OpenDroneID 编解码   | GitHub: opendroneid |
| libmav2odid        | MAVLink ↔ ODID 转换 | 项目子模块          |
| libcanard          | UAVCAN/CAN 协议栈    | GitHub: UAVCAN      |
| Monocypher 3.1.2   | Ed25519 签名验证     | monocypher.org      |
| tinf (uzlib)       | gzip 解压            | —                  |
| ArduinoJson        | JSON 生成            | Arduino 库管理器    |
| ESP32 Arduino Core | ESP32 平台支持       | espressif           |
