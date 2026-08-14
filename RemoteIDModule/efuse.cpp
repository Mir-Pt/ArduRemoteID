/*
  eFuse 安全熔丝配置实现

  通过烧录 ESP32 eFuse 位来禁用以下调试/下载接口，
  确保固件只能通过签名验证的 Web OTA 方式更新：
  - DIS_DOWNLOAD_MODE：禁用下载模式
  - DIS_USB_JTAG：禁用 USB JTAG 调试
  - DIS_PAD_JTAG：禁用引脚 JTAG（如可用）
  - DIS_USB_SERIAL_JTAG：禁用 USB 串口 JTAG
  - DIS_USB_DOWNLOAD_MODE：禁用 USB 下载模式
  - DIS_FORCE_DOWNLOAD：禁用强制下载
  - DIS_LEGACY_SPI_BOOT：禁用传统 SPI 引导

  触发条件：lock_level >= 2 且存在未烧录的 eFuse 位。
  注意：eFuse 一旦烧录不可撤销！
 */
#include <Arduino.h>
#include "efuse.h"
#include <soc/efuse_reg.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include "parameters.h"

// 需要烧录的 eFuse 位列表（不同 ESP32 型号支持的位不同，通过条件编译适配）
static const struct {
    const esp_efuse_desc_t **desc;  // eFuse 位描述符
    const char *name;               // eFuse 位名称（用于日志输出）
} fuses[] = {
    { ESP_EFUSE_DIS_DOWNLOAD_MODE, "DIS_DOWNLOAD_MODE" },
    { ESP_EFUSE_DIS_USB_JTAG, "DIS_USB_JTAG" },
#ifdef ESP_EFUSE_DIS_PAD_JTAG
    { ESP_EFUSE_DIS_PAD_JTAG, "DIS_PAD_JTAG" },
#endif
#ifdef ESP_EFUSE_DIS_USB_SERIAL_JTAG
    { ESP_EFUSE_DIS_USB_SERIAL_JTAG, "DIS_USB_SERIAL_JTAG" },
#endif
#ifdef ESP_EFUSE_DIS_USB_DOWNLOAD_MODE
    { ESP_EFUSE_DIS_USB_DOWNLOAD_MODE, "DIS_USB_DOWNLOAD_MODE" },
#endif
#ifdef ESP_EFUSE_DIS_FORCE_DOWNLOAD
    { ESP_EFUSE_DIS_FORCE_DOWNLOAD, "DIS_FORCE_DOWNLOAD" },
#endif
#ifdef ESP_EFUSE_DIS_LEGACY_SPI_BOOT
    { ESP_EFUSE_DIS_LEGACY_SPI_BOOT, "DIS_LEGACY_SPI_BOOT" },
#endif
};

/*
  set_efuses() - 检查并烧录安全 eFuse
  1. 遍历所有 eFuse 位，打印当前状态
  2. 如果 lock_level >= 2 且有未烧录的位，批量烧录
  烧录后设备将无法通过 JTAG 或下载模式刷写固件，
  只能通过签名验证的 Web OTA 接口更新。
 */
void set_efuses(void)
{
    bool some_unset = false;
    // 检查所有 eFuse 位的当前状态
    for (const auto &f : fuses) {
        const bool v = esp_efuse_read_field_bit(f.desc);
        Serial.printf("%s = %u\n", f.name, unsigned(v));
        some_unset |= !v;  // 记录是否存在未烧录的位
    }
    if (g.lock_level >= 2 && some_unset) {
        // 锁定等级 >= 2 且有未烧录的位：执行批量烧录
        Serial.printf("Burning efuses\n");
        esp_efuse_batch_write_begin();
        for (const auto &f : fuses) {
            const bool v = esp_efuse_read_field_bit(f.desc);
            if (!v) {
                Serial.printf("%s -> 1\n", f.name);
                auto ret = esp_efuse_write_field_bit(f.desc);
                if (ret != ESP_OK) {
                    Serial.printf("%s change failed\n", f.name);
                }
            }
        }
        esp_efuse_batch_write_commit();  // 提交所有 eFuse 修改
    }
}

