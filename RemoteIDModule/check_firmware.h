/*
  OTA 固件签名验证模块

  在 OTA（空中升级）过程中验证新固件的合法性：
  1. 在固件映像中查找应用描述符（app_descriptor）
  2. 使用 Ed25519 公钥验证描述符中的签名
  3. 验证板 ID 匹配（防止刷错固件）

  应用描述符使用反转的魔数（APP_DESCRIPTOR_REV），
  防止描述符模式本身出现在 Flash 中造成误匹配。
 */
#pragma once

#include "options.h"
#include <stdint.h>
#include <esp_ota_ops.h>

// 应用描述符魔数（反转存储，防止 Flash 中出现误匹配）
#define APP_DESCRIPTOR_REV { 0x19, 0x75, 0xe2, 0x46, 0x37, 0xf1, 0x2a, 0x43 }

// 固件签名验证类
class CheckFirmware {
public:
    // 应用描述符结构（嵌入在固件映像中）
    typedef struct {
        uint8_t sig[8];             // 魔数签名（匹配 APP_DESCRIPTOR_REV 的反转）
        uint32_t  board_id;         // 目标板 ID（必须与当前硬件匹配）
        uint32_t image_size;        // 固件映像大小（字节）
        uint8_t sign_signature[64]; // Ed25519 签名（对固件映像的数字签名）
    } app_descriptor_t;

    // 验证 OTA 目标分区上的固件签名（刷写前调用）
    static bool check_OTA_next(const esp_partition_t *part, const uint8_t *lead_bytes, uint32_t lead_length);
    // 验证当前运行分区的固件签名（启动时调用）
    static bool check_OTA_running(void);

private:
    // 在指定分区中查找并验证应用描述符
    static bool check_OTA_partition(const esp_partition_t *part, const uint8_t *lead_bytes, uint32_t lead_length, uint32_t &board_id);
    // 使用指定公钥验证固件签名
    static bool check_partition(const uint8_t *flash, uint32_t flash_len,
                                const uint8_t *lead_bytes, uint32_t lead_length,
                                const app_descriptor_t *ad, const uint8_t public_key[32]);
};
