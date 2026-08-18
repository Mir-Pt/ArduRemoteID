# flash_download_tool 3.9.9 烧录指南

> 适用：ESP32-C3 自研 RemoteID 板（4MB XMC Flash）
> 目标固件：ArduRemoteID（Arduino ESP32 Core 2.0.11 编译产物）
> 生成时间：2026-08-14

---

## 一、准备

### 1.1 工具

- 乐鑫官方烧录工具：`flash_download_tool_3.9.9_R2.exe`（Windows GUI，内置 esptool）

### 1.2 需要烧录的 4 个 bin 文件

| 顺序 | 文件 | 烧录地址 | 说明 |
|---|---|---|---|
| 1 | `RemoteIDModule.ino.bootloader.bin` | `0x0` | 二级引导程序 |
| 2 | `RemoteIDModule.ino.partitions.bin` | `0x8000` | 分区表 |
| 3 | `boot_app0.bin` | `0xe000` | OTA 引导信息（otadata 初始内容） |
| 4 | `RemoteIDModule.ino.bin` | `0x10000` | 应用程序固件 |

**文件路径**（前 3 个在编译产物目录，第 3 个在 ESP32 core 目录）：

```
# 编译产物目录（arduino-cli 编译后自动生成）
RemoteIDModule/build/esp32.esp32.esp32c3/RemoteIDModule.ino.bootloader.bin
RemoteIDModule/build/esp32.esp32.esp32c3/RemoteIDModule.ino.partitions.bin
RemoteIDModule/build/esp32.esp32.esp32c3/RemoteIDModule.ino.bin

# boot_app0.bin 不在编译产物里，在 ESP32 Arduino Core 目录（注意版本号 2.0.11）
C:/Users/Administrator/AppData/Local/Arduino15/packages/esp32/hardware/esp32/2.0.11/tools/partitions/boot_app0.bin
```

> ⚠️ **boot_app0.bin 的坑**：它不在 build 目录里。找不到时去
> `%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\<版本号>\tools\partitions\` 下找。

---

## 二、烧录配置

### 2.1 启动选择（第一屏）

双击 `flash_download_tool_3.9.9_R2.exe`：

| 选项 | 选择值 |
|---|---|
| **ChipType** | **ESP32-C3** |
| **WorkMode** | **Develop** |
| **LoadMode** | **UART** |

点 **OK** 进入下载配置界面。

### 2.2 SPI Flash 下载配置（第二屏，SPIDownload 标签页）

#### 顶部串口设置

| 项 | 设置 |
|---|---|
| **COM Port** | 选择模块连接的串口（板载 USB = 对应 COM 口） |
| **BAUDRATE** | `460800` 或 `921600`（不稳时降为 `115200`） |

#### bin 文件列表（4 行）

> ⚠️⚠️ **每个文件路径最前面的小方框（checkbox）必须打勾**！不勾 = 工具不会写这个文件，
> 但流程照样显示「FINISH 完成」，导致 Flash 实际是空的（全 `0xff`）——这就是「假成功」的最常见原因。

| # | 勾选 | 文件 | 偏移地址 @ |
|---|---|---|---|
| 1 | ✅ | `...RemoteIDModule.ino.bootloader.bin` | `0x0` |
| 2 | ✅ | `...RemoteIDModule.ino.partitions.bin` | `0x8000` |
| 3 | ✅ | `...boot_app0.bin` | `0xe000` |
| 4 | ✅ | `...RemoteIDModule.ino.bin` | `0x10000` |

#### SPI Flash 参数

| 项 | 设置 | 对应 esptool 参数 |
|---|---|---|
| **SPI SPEED** | `80MHz` | `--flash_freq 80m` |
| **SPI MODE** | **`DIO`** | `--flash_mode dio` |
| **FLASH SIZE** | `4MB`（或 `32Mbit`） | `--flash_size 4MB` |

> ⚠️ **SPI MODE 必须选 `DIO`，不要选 `QIO`**。工具 `DetectedInfo` 显示 `QUAD;4MB` 容易误导，
> 但这颗 XMC Flash（devID `4016h`）实测用 QIO 写不进去、用 DIO 才正常（与 esptool 命令行一致）。

#### 其他选项

| 选项 | 设置 | 说明 |
|---|---|---|
| **DoNotChgBin** | ✅ 勾选 | 按 bin 原样下载，不改写头部（强烈建议） |
| **EraseFlash** | ❌ 不勾选 | 勾选会整片擦除 Flash（含 NVS 参数） |
| **LockSettings / CombineBin** | ❌ 不勾选 | 默认即可 |

---

## 三、烧录步骤

1. 确认模块已通过 USB 连接到电脑（设备管理器出现 COM 口）。
2. 关闭占用该串口的程序（QGroundControl、串口监视器等，否则报 `PermissionError`）。
3. 按「二」完成所有配置，**确认 4 个文件都已勾选**。
4. 点 **START**，观察进度条是否真实走动（有写入百分比）。
5. 显示 **FINISH 完成** 后，按「四、烧录后自检」验证。
6. 验证通过后模块自动重启（或手动断电重上电）。

---

## 四、烧录后自检（强烈建议，防止「假成功」）

烧完不拆线，用 esptool 读回 `0x0` 验证是否真写进去了：

```bash
python -m esptool --chip esp32c3 --port COMxx read_flash 0x0 0x40 -
```

- 输出以 **`e9` 开头** → 真写进去了 ✅（bootloader header 正确）
- 输出全是 **`ff ff ff...`** → 没写进去 ❌（检查文件是否勾选、SPI MODE 是否 DIO）

---

## 五、注意事项

### 5.1 「假成功」三连排查

现象：工具显示「FINISH 完成」，但板子没反应、ROM bootloader 刷屏 `invalid header: 0xffffffff`。

按顺序排查：
1. **4 个 bin 文件前面的 checkbox 是否都勾了？**（最常见）
2. **SPI MODE 是不是 DIO？**（选成 QIO 会写不进去）
3. **SPI SPEED 是不是 80MHz？**（40MHz 也能用，但保持与 esptool 一致）

### 5.2 板载 USB-Serial-JTAG 的时序坑

ESP32-C3 板载 USB（VID:PID `303A:1001`）复位时 CDC 端点会重枚举，工具内置 esptool 可能报
`Write timeout` / `port doesn't exist`。应对：用 CP210x 接 UART0 下载，或降低波特率，或改用 Python esptool 5.3.1。

### 5.3 NVS 参数保留 vs 擦除

这 4 个 bin 的地址（`0x0 / 0x8000 / 0xe000 / 0x10000`）不覆盖 NVS（`0x9000`~`0xe000`），
不勾 EraseFlash 时参数（UAS_ID、WiFi、BAUDRATE 等）会保留。想恢复出厂则勾 EraseFlash。

---

## 附：与命令行 esptool 的等价关系

flash_download_tool 这套配置等价于：

```bash
python -m esptool --chip esp32c3 --port COMxx --baud 921600 \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     RemoteIDModule/build/esp32.esp32.esp32c3/RemoteIDModule.ino.bootloader.bin \
  0x8000  RemoteIDModule/build/esp32.esp32.esp32c3/RemoteIDModule.ino.partitions.bin \
  0xe000  C:/Users/Administrator/AppData/Local/Arduino15/packages/esp32/hardware/esp32/2.0.11/tools/partitions/boot_app0.bin \
  0x10000 RemoteIDModule/build/esp32.esp32.esp32c3/RemoteIDModule.ino.bin
```
