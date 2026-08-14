# ArduRemoteID ESP32-S3 编译上传完整记录

## 项目概述

ArduRemoteID 是一个开源的 OpenDroneID（开放无人机远程识别）实现，运行在 ESP32 系列芯片上，支持通过 WiFi NAN、WiFi Beacon、BLE 4 和 BLE 5 广播无人机识别信息，同时支持 MAVLink 和 DroneCAN 协议接收飞控数据。

- 项目地址：`C:\Users\Administrator\Documents\github\ArduRemoteID`
- 目标板：ESP32-S3（Holybro RemoteID 模块，CH343 USB转串口芯片）
- 串口：COM20
- 编译工具：arduino-cli
- ESP32 Arduino Core 版本：2.0.11

---

## 一、环境准备

### 1.1 Arduino CLI 安装

使用 arduino-cli 作为命令行编译工具，避免 Arduino IDE 的图形界面限制。

### 1.2 ESP32 Board 支持包

通过 arduino-cli 的 board manager 安装 esp32 核心包（2.0.11 版本）。板管理器 URL 配置在项目的 `arduino-cli.yaml` 中。

### 1.3 Python 环境

Windows 系统自带的 python/python3 命令是 App Store 的占位符，无法实际执行。解决方案是使用 ESP-IDF 工具链自带的 Python：

```
C:\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe
```

---

## 二、代码生成（预编译步骤）

### 2.1 MAVLink 协议头文件生成

**为什么需要：** ArduRemoteID 使用 MAVLink 协议与飞控通信，需要从 XML 协议定义文件生成 C 语言头文件。

**操作步骤：**
```bash
git submodule update --init --recursive
pip install pymavlink
python -m pymavlink.tools.mavgen --lang=C --wire-protocol=2.0 \
    --output=libraries/mavlink2/generated \
    modules/mavlink/message_definitions/v1.0/all.xml
```

**遇到的问题：**
- `pymavlink` 未安装 → `pip install pymavlink`
- `modules/mavlink` 子模块未初始化 → `git submodule update --init --recursive`

### 2.2 DroneCAN DSDL 代码生成

**为什么需要：** DroneCAN 协议的数据类型定义（DSDL）需要通过代码生成器转换为 C 语言的编解码函数。

**操作步骤：**
```bash
pip install dronecan pexpect "empy==3.3.4"
python modules/dronecan/dronecan_dsdlc/dronecan_dsdlc.py \
    -O libraries/DroneCAN/dsdl_generated \
    modules/dronecan/DSDL/dronecan modules/dronecan/DSDL/uavcan \
    modules/dronecan/DSDL/com modules/dronecan/DSDL/ardupilot
```

**遇到的问题：**
- `empy` 模块未安装 → `pip install empy`
- `dronecan` 模块未安装 → `pip install dronecan`
- `pexpect` 模块未安装 → `pip install pexpect`
- **empy 4.x 不兼容**：empy 4.2.1 版本移除了旧版 `@)` 模板语法，导致 dronecan_dsdlc 模板解析失败。解决方案：降级到 3.3.4 版本
  ```bash
  pip install "empy==3.3.4" --force-reinstall
  ```

### 2.3 ROMFS 网页资源嵌入

**为什么需要：** Web 管理界面的 HTML/JS/CSS/图片文件需要嵌入到固件中，通过 `scripts/make_romfs.py` 将文件压缩为 gzip 格式的 C 数组。

### 2.4 Git 版本号

**为什么需要：** 固件需要包含 git commit hash 作为版本标识。

手动创建 `RemoteIDModule/git-version.h`：
```c
#define GIT_VERSION 0x7033684
```

---

## 三、库依赖处理

### 3.1 通过 arduino-cli 安装的库

| 库名 | 版本 | 用途 |
|------|------|------|
| ArduinoJson | 7.4.3 | JSON 解析（Web 接口参数交互） |
| Adafruit NeoPixel | 1.15.4 | WS2812 LED 状态指示灯驱动 |

安装命令：
```bash
arduino-cli lib install "ArduinoJson"
arduino-cli lib install "Adafruit NeoPixel"
```

### 3.2 手动创建的 Arduino 库

#### libcanard 库

**为什么需要：** DroneCAN 协议栈依赖 libcanard 库（轻量级 CAN 协议实现），项目中以 git submodule 形式存在于 `modules/libcanard/`，但没有 Arduino 库结构，编译器找不到头文件。

**解决方案：** 创建标准 Arduino 库结构：

```
~/Documents/Arduino/libraries/libcanard/
├── library.properties
└── src/
    ├── canard.h
    ├── canard_internals.h
    └── canard.c
```

`library.properties` 内容：
```ini
name=libcanard
version=1.0.0
author=UAVCAN
sentence=Lightweight CAN library
paragraph=Lightweight CAN library for DroneCAN
category=Communication
architectures=*
```

#### DroneCAN_generated 库

**为什么需要：** DSDL 代码生成器产出的 .h 和 .c 文件需要被编译器发现并编译。最初尝试将源文件复制到 sketch 目录，但由于代码中使用 `#include <header.h>`（尖括号形式），sketch 目录中的文件无法被找到。

**解决方案：** 创建标准 Arduino 库：

```
~/Documents/Arduino/libraries/DroneCAN_generated/
├── library.properties
└── src/
    ├── *.h (所有生成的头文件)
    └── *.c (所有生成的源文件)
```

**关键理解：** Arduino 编译系统中，`<>` 尖括号 include 只搜索库路径，不搜索 sketch 目录。将代码做成正式的 Arduino 库后，其 `src/` 目录会被加入编译器的 include 搜索路径。

---

## 四、代码修改

### 4.1 BLE_TX.cpp — 移除不兼容的功率等级宏

**文件：** `RemoteIDModule/BLE_TX.cpp`

**问题：** `ESP_PWR_LVL_N27` 和 `ESP_PWR_LVL_N24` 宏仅在 ESP32 Arduino Core 3.x 中定义，2.0.11 版本没有这两个宏，导致编译错误。

**修改：** 从 `dBm_to_tx_power()` 函数的查找表中移除这两个条目：

```cpp
// 修改前
} dBm_table[] = {
    { ESP_PWR_LVL_N27,-27 },
    { ESP_PWR_LVL_N24,-24 },
    { ESP_PWR_LVL_N21,-21 },
    ...
};

// 修改后
} dBm_table[] = {
    { ESP_PWR_LVL_N21,-21 },
    ...
};
```

**影响：** BLE 最低发射功率从 -27dBm 变为 -21dBm，实际使用中影响极小（通常不会设置如此低的功率）。

### 4.2 RemoteIDModule.ino — 固定串口波特率

**文件：** `RemoteIDModule/RemoteIDModule.ino`

**问题：** 原代码使用 `g.baudrate`（参数系统中的值，默认 57600）作为串口波特率。但 ESP32-S3 的 USB CDC 串口固定为 115200，且 NVS 中可能存储了其他值，导致串口输出乱码。

**修改：**
```cpp
// 修改前
Serial.begin(g.baudrate);
Serial1.begin(g.baudrate, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

// 修改后
Serial.begin(115200);
Serial1.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
```

**原因：** USB CDC 接口的波特率由 USB 协议决定，设置其他值无意义。MAVLink 串口（Serial1）也统一使用 115200 以匹配常见飞控配置。

### 4.3 parameters.h — 设置默认 WiFi SSID

**文件：** `RemoteIDModule/parameters.h`

**问题：** 默认 `wifi_ssid` 为空字符串 `""`，导致 WiFi AP 不会广播，用户无法发现和连接。

**修改：**
```cpp
// 修改前
char wifi_ssid[21] = "";

// 修改后
char wifi_ssid[21] = "ArduRemoteID";
```

**原因：** 新烧录的固件 NVS 为空，所有参数使用默认值。如果 SSID 默认为空，WiFi AP 不会启动，用户无法通过 Web 界面进行初始配置。

---

## 五、编译配置

### 5.1 分区表问题

**问题：** 默认 ESP32 分区表只给应用分配 1.3MB（1,310,720 字节），而 ArduRemoteID 固件编译后约 1.49MB，超出限制。

**解决方案：** 使用项目自带的 `partitions.csv`，通过编译参数指定：

```
--build-property "build.partitions=partitions"
--build-property "upload.maximum_size=2031616"
--build-property "build.custom_partitions=C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule/partitions.csv"
```

项目分区表分配：
| 分区名 | 类型 | 偏移 | 大小 |
|--------|------|------|------|
| nvs | data/nvs | 0x9000 | 0x5000 (20KB) |
| otadata | data/ota | 0xe000 | 0x2000 (8KB) |
| app0 | app/ota_0 | 0x10000 | 0x1f0000 (1.9MB) |
| app1 | app/ota_1 | 0x200000 | 0x1f0000 (1.9MB) |
| param | 0x46 | 0x3f0000 | 0x4000 (16KB) |

### 5.2 最终编译命令

```bash
arduino-cli compile \
    --fqbn esp32:esp32:esp32s3 \
    --libraries "C:/Users/Administrator/Documents/github/ArduRemoteID/libraries" \
    --build-property "build.extra_flags=-DBOARD_Holybro_RemoteID -DESP32" \
    --build-property "build.partitions=partitions" \
    --build-property "upload.maximum_size=2031616" \
    --build-property "build.custom_partitions=C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule/partitions.csv" \
    --upload --port COM20 \
    C:/Users/Administrator/Documents/github/ArduRemoteID/RemoteIDModule
```

**参数说明：**
- `--fqbn esp32:esp32:esp32s3`：目标板为 ESP32-S3
- `--libraries`：指定项目内部库路径（mavlink2、DroneCAN 等）
- `-DBOARD_Holybro_RemoteID`：选择 Holybro RemoteID 板的引脚配置
- `-DESP32`：启用 ESP32 相关条件编译
- `--upload --port COM20`：编译后自动上传到 COM20

---

## 六、上传与验证

### 6.1 上传结果

固件成功上传，串口输出确认：

```
ArduRemoteID 1.14 d7033684
CAN driver started
WiFi AP started: ArduRemoteID
Web server running on 192.168.4.1
```

### 6.2 Web 管理界面访问

1. 电脑连接 WiFi 热点 `ArduRemoteID`，密码 `ArduRemoteID`
2. 浏览器访问 `http://192.168.4.1`
3. 可配置参数：UAS ID、广播速率、功率、WiFi 设置等

---

## 七、问题总结与经验

| 问题类别 | 具体问题 | 根本原因 | 解决方案 |
|----------|----------|----------|----------|
| 环境 | Python 不可用 | Windows App Store 占位符 | 使用 ESP-IDF 自带 Python |
| 依赖 | pymavlink/dronecan/empy 缺失 | 代码生成器的 Python 依赖 | pip install |
| 兼容性 | empy 4.x 模板解析失败 | 新版 empy 移除旧语法 | 降级到 3.3.4 |
| 编译 | canard.h 找不到 | 子模块无 Arduino 库结构 | 创建 Arduino 库 |
| 编译 | DroneCAN .c 文件未编译 | sketch 目录不支持 `<>` include | 做成 Arduino 库 |
| 编译 | ESP_PWR_LVL_N27 未定义 | Core 2.0.11 无此宏 | 移除不支持的功率等级 |
| 链接 | 固件超出分区大小 | 默认分区仅 1.3MB | 使用项目自定义分区表 |
| 运行 | 串口乱码 | 波特率不匹配 | 固定 115200 |
| 运行 | WiFi AP 不可见 | 默认 SSID 为空 | 设置默认值 |

---

## 八、项目架构理解

```
ArduRemoteID/
├── RemoteIDModule/          # 主 sketch
│   ├── RemoteIDModule.ino   # 入口（setup/loop）
│   ├── board_config.h       # 各板型引脚定义
│   ├── parameters.h/cpp     # NVS 参数系统
│   ├── mavlink.h/cpp        # MAVLink 协议处理
│   ├── DroneCAN.h/cpp       # DroneCAN 协议处理
│   ├── WiFi_TX.h/cpp        # WiFi NAN/Beacon 广播
│   ├── BLE_TX.h/cpp         # BLE 4/5 广播
│   ├── webinterface.h/cpp   # Web 配置界面
│   ├── led.h/cpp            # LED 状态指示
│   ├── partitions.csv       # 自定义分区表
│   └── romfs.h              # 嵌入式网页资源
├── libraries/
│   ├── mavlink2/            # MAVLink 生成代码
│   └── DroneCAN/            # DroneCAN 库
├── modules/                 # Git 子模块
│   ├── mavlink/             # MAVLink 协议定义
│   ├── dronecan/            # DroneCAN DSDL + 生成器
│   └── libcanard/           # CAN 协议栈
├── scripts/                 # 构建辅助脚本
│   ├── regen_headers.sh     # 头文件生成
│   ├── make_romfs.py        # 网页资源嵌入
│   └── git-version.sh       # 版本号生成
└── web/                     # Web 界面源文件
```

---

## 九、注意事项

1. **Makefile 中的板型配置**：项目 Makefile 中 Holybro RemoteID 目标使用 `CHIP=esp32c3`，但实际硬件是 ESP32-S3。编译时需手动指定 `--fqbn esp32:esp32:esp32s3`。

2. **NVS 参数持久化**：首次烧录后参数使用代码中的默认值。通过 Web 界面或 MAVLink 修改的参数会保存到 NVS 分区，后续启动优先使用 NVS 中的值。

3. **OTA 支持**：分区表包含两个 app 分区（app0/app1），支持 OTA 空中升级。`esp_ota_mark_app_valid_cancel_rollback()` 确认当前固件有效，防止回滚。

4. **Core 版本兼容性**：本次编译使用 ESP32 Arduino Core 2.0.11。如升级到 3.x 版本，BLE_TX.cpp 的修改可以还原（恢复 N27/N24 功率等级）。
