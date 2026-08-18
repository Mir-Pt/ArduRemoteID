# ArduRemoteID Arduino-CLI 编译指南

## 环境信息

| 项目 | 值 |
|------|------|
| arduino-cli 版本 | 1.4.1 |
| ESP32 核心版本 | 2.0.11 |
| 目标板 FQBN | esp32:esp32:esp32s3 |
| 目标板型号 | Holybro RemoteID |
| MCU | ESP32-S3 |
| 项目路径 | `C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule` |

---

## 一、编译命令

### 仅编译（不上传）

```bat
"C:\Program Files\Arduino CLI\arduino-cli.exe" compile ^
  --fqbn esp32:esp32:esp32s3 ^
  --libraries "C:\Users\Administrator\Documents\github\ArduRemoteID\libraries" ^
  --build-property "build.extra_flags=-DBOARD_Holybro_RemoteID -DESP32" ^
  --build-property "build.partitions=partitions" ^
  --build-property "upload.maximum_size=2031616" ^
  --build-property "build.custom_partitions=C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule\partitions.csv" ^
  "C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule"
```

### 编译并上传

```bat
"C:\Program Files\Arduino CLI\arduino-cli.exe" compile ^
  --fqbn esp32:esp32:esp32s3 ^
  --libraries "C:\Users\Administrator\Documents\github\ArduRemoteID\libraries" ^
  --build-property "build.extra_flags=-DBOARD_Holybro_RemoteID -DESP32" ^
  --build-property "build.partitions=partitions" ^
  --build-property "upload.maximum_size=2031616" ^
  --build-property "build.custom_partitions=C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule\partitions.csv" ^
  --upload --port COM20 ^
  "C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule"
```

> 注意：将 `COM20` 替换为实际连接的串口号。

### 导出编译产物到 sketch 目录

添加 `--export-binaries` 或 `-e` 参数，编译产物（.bin 文件）会保存到 sketch 目录下的 `build/` 文件夹：

```bat
"C:\Program Files\Arduino CLI\arduino-cli.exe" compile ^
  --fqbn esp32:esp32:esp32s3 ^
  --libraries "C:\Users\Administrator\Documents\github\ArduRemoteID\libraries" ^
  --build-property "build.extra_flags=-DBOARD_Holybro_RemoteID -DESP32" ^
  --build-property "build.partitions=partitions" ^
  --build-property "upload.maximum_size=2031616" ^
  --build-property "build.custom_partitions=C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule\partitions.csv" ^
  --export-binaries ^
  "C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule"
```

---

## 二、参数说明

### 核心参数

| 参数 | 说明 |
|------|------|
| `--fqbn esp32:esp32:esp32s3` | 指定目标板为 ESP32-S3 Dev Module |
| `--libraries <path>` | 指定额外库搜索路径（项目内部库） |
| `--upload` | 编译后自动上传 |
| `--port COMx` | 指定上传串口 |
| `--export-binaries` | 将 .bin 文件导出到 sketch 目录 |
| `--clean` | 清除缓存，完全重新编译 |
| `--verbose` / `-v` | 显示详细编译日志 |

### build-property 参数

| 属性 | 值 | 说明 |
|------|------|------|
| `build.extra_flags` | `-DBOARD_Holybro_RemoteID -DESP32` | 定义编译宏，选择板型配置 |
| `build.partitions` | `partitions` | 分区表名称 |
| `upload.maximum_size` | `2031616` | 最大固件大小（对应 OTA 分区 0x1f0000） |
| `build.custom_partitions` | `<path>/partitions.csv` | 自定义分区表文件路径 |

---

## 三、分区表配置

### 3.1 ESP32 分区表基础

ESP32 使用分区表来划分 Flash 存储空间。分区表是一个 CSV 文件，定义了每个分区的名称、类型、偏移地址和大小。

#### CSV 格式

```
# Name,   Type, SubType, Offset,  Size,   Flags
```

| 列名 | 说明 |
|------|------|
| Name | 分区名称，最长 16 字符 |
| Type | 分区类型：`app`（应用程序）或 `data`（数据），也可用数字 0x00-0xFF |
| SubType | 子类型，取决于 Type |
| Offset | Flash 中的起始地址（十六进制），必须 4KB 对齐 |
| Size | 分区大小（十六进制），必须 4KB 对齐 |
| Flags | 可选标志，通常留空 |

#### 分区类型与子类型

**app 类型（Type=app/0x00）：**

| SubType | 值 | 说明 |
|---------|------|------|
| factory | 0x00 | 出厂固件（无 OTA 时使用） |
| ota_0 | 0x10 | OTA 分区 0 |
| ota_1 | 0x11 | OTA 分区 1 |
| ota_2 ~ ota_15 | 0x12~0x1F | 更多 OTA 分区（最多 16 个） |

**data 类型（Type=data/0x01）：**

| SubType | 值 | 说明 |
|---------|------|------|
| nvs | 0x02 | NVS 非易失性存储 |
| ota | 0x00 | OTA 选择数据（记录当前启动分区） |
| phy | 0x01 | PHY 初始化数据 |
| nvs_keys | 0x04 | NVS 加密密钥 |
| coredump | 0x03 | 核心转储 |
| spiffs | 0x82 | SPIFFS 文件系统 |

#### 关键约束

- 分区表本身位于 Flash 偏移 0x8000，大小 0x1000（4KB）
- 第一个 app 分区的最小偏移为 0x10000（64KB），前面是 bootloader + 分区表 + NVS
- OTA 需要至少两个 app 分区（ota_0 + ota_1）和一个 otadata 分区
- 所有分区不能重叠，且总大小不能超过 Flash 容量（通常 4MB = 0x400000）

---

### 3.2 ESP32 内置分区方案对比

ESP32 核心提供多种预定义分区方案，位于：
```
C:\Users\Administrator\AppData\Local\Arduino15\packages\esp32\hardware\esp32\2.0.11\tools\partitions\
```

| 方案名 | app 分区大小 | OTA | 适用场景 |
|--------|-------------|-----|----------|
| default (4MB) | 1.25 MB (0x140000) | 双分区 | 通用，带 SPIFFS |
| min_spiffs | 1.875 MB (0x1E0000) | 双分区 | 大固件 + 最小文件系统 |
| no_ota | 2 MB (0x200000) | 无 | 最大单固件，无 OTA |
| huge_app | 3 MB (0x300000) | 无 | 超大固件，无 OTA |

#### default.csv（默认方案）
```
# Name,   Type, SubType, Offset,  Size,   Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x140000,
app1,     app,  ota_1,   0x150000,0x140000,
spiffs,   data, spiffs,  0x290000,0x160000,
coredump, data, coredump,0x3F0000,0x10000,
```

#### min_spiffs.csv
```
# Name,   Type, SubType, Offset,  Size,   Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1E0000,
app1,     app,  ota_1,   0x1F0000,0x1E0000,
spiffs,   data, spiffs,  0x3D0000,0x20000,
coredump, data, coredump,0x3F0000,0x10000,
```

#### no_ota.csv
```
# Name,   Type, SubType, Offset,  Size,   Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  factory, 0x10000, 0x200000,
spiffs,   data, spiffs,  0x210000,0x1E0000,
coredump, data, coredump,0x3F0000,0x10000,
```

#### huge_app.csv
```
# Name,   Type, SubType, Offset,  Size,   Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  factory, 0x10000, 0x300000,
spiffs,   data, spiffs,  0x310000,0xE0000,
coredump, data, coredump,0x3F0000,0x10000,
```

---

### 3.3 ArduRemoteID 自定义分区表

项目使用自定义分区表 `RemoteIDModule/partitions.csv`，支持 OTA 双分区：

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1f0000,
app1,     app,  ota_1,   0x200000, 0x1f0000,
param,    0x46, 0,       0x3f0000, 0x4000,
```

| 分区名 | 类型 | 偏移 | 大小 | 说明 |
|--------|------|------|------|------|
| nvs | data/nvs | 0x9000 | 0x5000 (20KB) | 参数存储 |
| otadata | data/ota | 0xe000 | 0x2000 (8KB) | OTA 启动选择 |
| app0 | app/ota_0 | 0x10000 | 0x1f0000 (1984KB) | 主固件 |
| app1 | app/ota_1 | 0x200000 | 0x1f0000 (1984KB) | OTA 备份固件 |
| param | 0x46/0 | 0x3f0000 | 0x4000 (16KB) | 自定义参数分区 |

**为什么需要自定义分区表：**

ArduRemoteID 编译后固件约 1.49 MB（1,492,789 字节），超过了默认分区方案的 1.25 MB 限制。自定义分区将 app 分区扩大到 1984 KB（约 1.94 MB），同时保留 OTA 双分区能力，并去掉了 SPIFFS 文件系统分区（项目不需要），腾出空间给更大的固件。

---

### 3.4 arduino-cli 中使用自定义分区表

需要三个 `--build-property` 参数配合使用：

```bat
--build-property "build.partitions=partitions"
--build-property "upload.maximum_size=2031616"
--build-property "build.custom_partitions=C:\...\partitions.csv"
```

| 属性 | 说明 |
|------|------|
| `build.partitions` | 分区表名称（不含 .csv 后缀），用于生成分区表二进制文件 |
| `upload.maximum_size` | 最大固件大小（字节），必须与 app 分区 Size 一致。0x1f0000 = 2,031,616 |
| `build.custom_partitions` | 自定义分区表 CSV 文件的完整路径 |

如果只设置 `build.partitions` 而不设置 `build.custom_partitions`，arduino-cli 会在 ESP32 核心的 `tools/partitions/` 目录下查找对应的 CSV 文件。

#### upload.maximum_size 计算方法

`upload.maximum_size` 的值必须等于分区表中 app 分区的 Size 字段：

```
app0 分区 Size = 0x1f0000 = 2,031,616 字节
```

如果固件超过此值，编译器会报错：
```
Sketch too big; see https://support.arduino.cc/hc/en-us/articles/360013825179 for tips on reducing it.
```

---

### 3.5 使用内置分区方案（通过 FQBN 选项）

如果不需要自定义分区表，可以通过 FQBN 的 `PartitionScheme` 选项选择内置方案：

```bat
--fqbn "esp32:esp32:esp32s3:PartitionScheme=min_spiffs"
```

常用内置方案选项值：

| PartitionScheme 值 | 方案 | app 最大大小 |
|-------------------|------|-------------|
| `default` | Default 4MB with spiffs | 1,310,720 (0x140000) |
| `min_spiffs` | Minimal SPIFFS | 1,966,080 (0x1E0000) |
| `no_ota` | No OTA (2MB APP) | 2,097,152 (0x200000) |
| `huge_app` | Huge APP (3MB No OTA) | 3,145,728 (0x300000) |
| `default_8MB` | 8M with spiffs | 3,342,336 (0x330000) |
| `default_16MB` | 16M with spiffs | 6,553,600 (0x640000) |

使用 FQBN 选项时，不需要手动设置 `build.partitions`、`upload.maximum_size` 和 `build.custom_partitions`，这些值由 boards.txt 自动配置。

---

### 3.6 boards.txt 与分区方案的关系

ESP32 核心的 `boards.txt` 文件定义了每种分区方案对应的参数：

```properties
esp32s3.menu.PartitionScheme.default=Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)
esp32s3.menu.PartitionScheme.default.build.partitions=default
esp32s3.menu.PartitionScheme.default.upload.maximum_size=1310720

esp32s3.menu.PartitionScheme.min_spiffs=Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)
esp32s3.menu.PartitionScheme.min_spiffs.build.partitions=min_spiffs
esp32s3.menu.PartitionScheme.min_spiffs.upload.maximum_size=1966080
```

当通过 FQBN 选择 `PartitionScheme=min_spiffs` 时，arduino-cli 会：
1. 设置 `build.partitions=min_spiffs`
2. 设置 `upload.maximum_size=1966080`
3. 在 `tools/partitions/` 目录下查找 `min_spiffs.csv`
4. 将 CSV 编译为二进制分区表文件（.bin）写入 Flash 的 0x8000 位置

---

### 3.7 创建自定义分区表的步骤

1. **规划分区布局**：确定是否需要 OTA、SPIFFS、自定义数据分区等
2. **计算大小**：确保所有分区不重叠且总和不超过 Flash 容量（4MB = 0x400000）
3. **编写 CSV 文件**：按格式填写各分区信息
4. **设置编译参数**：
   - `build.partitions` = CSV 文件名（不含后缀）
   - `upload.maximum_size` = app 分区的 Size（十进制字节数）
   - `build.custom_partitions` = CSV 文件完整路径
5. **验证**：编译时检查固件大小是否在限制内

#### Flash 布局示意（4MB Flash）

```
0x000000 ┌──────────────────────┐
         │ Bootloader (2nd)     │ (由 ESP-IDF 管理)
0x008000 ├──────────────────────┤
         │ Partition Table      │ 0x1000 (4KB)
0x009000 ├──────────────────────┤
         │ NVS                  │ 0x5000 (20KB)
0x00E000 ├──────────────────────┤
         │ OTA Data             │ 0x2000 (8KB)
0x010000 ├──────────────────────┤
         │ app0 (ota_0)         │ 0x1F0000 (1984KB)
0x200000 ├──────────────────────┤
         │ app1 (ota_1)         │ 0x1F0000 (1984KB)
0x3F0000 ├──────────────────────┤
         │ param                │ 0x4000 (16KB)
0x3F4000 ├──────────────────────┤
         │ (未使用)              │ 0xC000 (48KB)
0x400000 └──────────────────────┘
```

---

## 四、项目库依赖

项目 `libraries/` 目录下包含以下内部库：

| 库名 | 说明 |
|------|------|
| mavlink2 | MAVLink v2 协议库 |
| mavlink_c_library_v2 | MAVLink C 库源码 |
| libopendroneid | OpenDroneID 核心协议实现 |
| libmav2odid | MAVLink 到 OpenDroneID 转换 |
| DroneCAN_generated | DroneCAN 自动生成的消息定义 |

这些库通过 `--libraries` 参数指定路径，arduino-cli 会自动搜索该目录下的所有子库。

Arduino IDE 的 sketchbook libraries 目录（`C:\Users\Administrator\Documents\Arduino\libraries\`）中还安装了：

| 库名 | 版本 |
|------|------|
| Adafruit NeoPixel | 1.15.4 |
| ArduinoJson | 7.4.3 |
| TFT_eSPI | 2.5.43 |
| libcanard | 1.0.0 |

---

## 五、支持的板型配置

通过 `build.extra_flags` 中的 `-DBOARD_xxx` 宏选择不同硬件：

| 宏定义 | Board ID | 说明 |
|--------|----------|------|
| `BOARD_ESP32S3_DEV` | 1 | ESP32-S3 开发板 |
| `BOARD_ESP32C3_DEV` | 2 | ESP32-C3 开发板 |
| `BOARD_BLUEMARK_DB200` | 3 | BlueMark DB200 |
| `BOARD_BLUEMARK_DB110` | 4 | BlueMark DB110 |
| `BOARD_JW_TBD` | 5 | JW TBD |
| `BOARD_MRO_RID` | 6 | mRobotics RemoteID |
| `BOARD_JWRID_ESP32S3` | 7 | JWRID ESP32-S3 |
| `BOARD_BLUEMARK_DB202` | 8 | BlueMark DB202 |
| `BOARD_BLUEMARK_DB203` | 9 | BlueMark DB203 |
| `BOARD_Holybro_RemoteID` | 11 | Holybro RemoteID（当前使用） |
| `BOARD_CUAV_RID` | 12 | CUAV C-RID |

切换板型时修改 `build.extra_flags` 中的宏即可，例如：
```
--build-property "build.extra_flags=-DBOARD_CUAV_RID -DESP32"
```

---

## 六、ESP32-S3 板级选项（FQBN 可选参数）

可通过 FQBN 附加选项调整硬件配置：

```
esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=0,FlashSize=4M,PartitionScheme=default,...
```

常用选项：

| 选项 | 默认值 | 可选值 |
|------|--------|--------|
| FlashMode | qio | qio, dio, opi |
| FlashSize | 4MB | 4MB, 8MB, 16MB, 32MB |
| CPUFreq | 240MHz | 240/160/80/40/20/10 MHz |
| UploadSpeed | 921600 | 921600, 512000, 256000, 230400, 115200 |
| USBMode | hwcdc | hwcdc, default(TinyUSB) |
| CDCOnBoot | 0 | 0(禁用), 1(启用) |

---

## 七、常用操作

### 查看可用串口
```bat
"C:\Program Files\Arduino CLI\arduino-cli.exe" board list
```

### 清除编译缓存后重新编译
在编译命令中添加 `--clean` 参数。

### 查看详细编译日志
在编译命令中添加 `--verbose` 参数。

### 仅上传（不重新编译）
```bat
"C:\Program Files\Arduino CLI\arduino-cli.exe" upload ^
  --fqbn esp32:esp32:esp32s3 ^
  --port COM20 ^
  "C:\Users\Administrator\Documents\github\ArduRemoteID\RemoteIDModule"
```

---

## 八、为什么不用 Arduino IDE

Arduino IDE 无法编译此项目，原因：

1. **不支持 `--libraries` 参数** — 项目内部库（mavlink2 等）无法被 IDE 自动发现
2. **间接 include 不触发库搜索** — IDE 只对 `.ino` 文件中的 `#include` 做库匹配，`.h` 文件中的间接引用不会触发
3. **mavlink2 库结构特殊** — 包含嵌套的 `generated/` 子目录，IDE 的库管理器不能正确处理

arduino-cli 提供了完整的命令行控制，适合此类复杂项目的编译和自动化。

---

## 九、编译结果参考

最近一次成功编译输出：

```
Sketch uses 1492789 bytes (73%) of program storage space. Maximum is 2031616 bytes.
Global variables use 74540 bytes (22%) of dynamic memory, leaving 253140 bytes for local variables. Maximum is 327680 bytes.
```
