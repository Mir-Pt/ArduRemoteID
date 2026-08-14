/*
  OTA 固件签名验证实现

  在 OTA（空中升级）过程中验证新固件的合法性：
  1. 将固件分区映射到内存
  2. 在映像中搜索应用描述符（反转魔数匹配）
  3. 验证描述符中的映像大小是否与实际偏移一致
  4. 使用所有已配置的 Ed25519 公钥逐个尝试验证签名
  5. 验证板 ID 是否匹配当前硬件

  安全策略：
  - 无公钥时接受任何固件（方便开发）
  - lock_level == -1 时跳过签名验证（调试模式）
  - 板 ID 不匹配时拒绝（防止刷错固件）
 */
#include <Arduino.h>
#include "check_firmware.h"
#include "monocypher.h"
#include "parameters.h"
#include <string.h>
#include "util.h"

/*
  check_partition() - 使用指定公钥验证固件签名
  通过 Monocypher 的 Ed25519 增量验证 API 计算签名：
  1. 初始化验证上下文（签名 + 公钥）
  2. 先输入 lead_bytes（OTA 缓存的前几字节）
  3. 再输入 Flash 中的剩余数据
  4. 最终化并检查签名是否匹配
 */
bool CheckFirmware::check_partition(const uint8_t *flash, uint32_t flash_len,
                                    const uint8_t *lead_bytes, uint32_t lead_length,
                                    const app_descriptor_t *ad, const uint8_t public_key[32])
{
    crypto_check_ctx ctx {};
    crypto_check_ctx_abstract *actx = (crypto_check_ctx_abstract*)&ctx;
    crypto_check_init(actx, ad->sign_signature, public_key);
    if (lead_length > 0) {
        crypto_check_update(actx, lead_bytes, lead_length);
    }
    crypto_check_update(actx, &flash[lead_length], flash_len-lead_length);
    return crypto_check_final(actx) == 0;
}

/*
  check_OTA_partition() - 验证指定分区上的固件
  1. 将 Flash 分区映射到内存（mmap）
  2. 搜索应用描述符魔数（反转后匹配）
  3. 验证描述符中记录的映像大小与实际偏移一致
  4. 如果没有配置公钥，直接接受固件
  5. 否则逐个公钥尝试验证签名
 */
bool CheckFirmware::check_OTA_partition(const esp_partition_t *part, const uint8_t *lead_bytes, uint32_t lead_length, uint32_t &board_id)
{
    Serial.printf("Checking partition %s\n", part->label);
    // 将 Flash 分区映射到内存地址空间
    spi_flash_mmap_handle_t handle;
    const void *ptr = nullptr;
    auto ret = esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_DATA, &ptr, &handle);
    if (ret != ESP_OK) {
        Serial.printf("mmap failed\n");
        return false;
    }
    // 构造正序魔数（从反转魔数翻转）
    const uint8_t sig_rev[] = APP_DESCRIPTOR_REV;
    uint8_t sig[8];
    for (uint8_t i=0; i<8; i++) {
        sig[i] = sig_rev[7-i];
    }
    // 在映像中搜索应用描述符
    const app_descriptor_t *ad = (app_descriptor_t *)memmem(ptr, part->size, sig, sizeof(sig));
    if (ad == nullptr) {
        Serial.printf("app_descriptor not found\n");
        spi_flash_munmap(handle);
        return false;
    }
    Serial.printf("app descriptor at 0x%x size=%u id=%u (own id %u)\n", unsigned(ad)-unsigned(ptr), ad->image_size, ad->board_id,BOARD_ID);
    // 验证描述符中的映像大小与实际偏移一致
    const uint32_t img_len = uint32_t(uintptr_t(ad) - uintptr_t(ptr));
    if (ad->image_size != img_len) {
        Serial.printf("app_descriptor bad size %u\n", ad->image_size);
        spi_flash_munmap(handle);
        return false;
    }
    board_id = ad->board_id;

    // 无公钥时直接接受固件（方便开发阶段）
    if (g.no_public_keys()) {
        Serial.printf("No public keys - accepting firmware\n");
        spi_flash_munmap(handle);
        return true;
    }

    // 逐个公钥尝试验证签名
    for (uint8_t i=0; i<MAX_PUBLIC_KEYS; i++) {
        uint8_t key[32];
        if (!g.get_public_key(i, key)) {
            continue;
        }
        if (check_partition((const uint8_t *)ptr, img_len, lead_bytes, lead_length, ad, key)) {
            Serial.printf("check firmware good for key %u\n", i);
            spi_flash_munmap(handle);
            return true;
        }
        Serial.printf("check failed key %u\n", i);
    }
    spi_flash_munmap(handle);
    Serial.printf("firmware failed checks\n");
    return false;
}

/*
  check_OTA_next() - 验证 OTA 目标分区上的固件（刷写前调用）
  - lock_level == -1：调试模式，接受任何固件
  - 验证板 ID 匹配（非零时必须与当前硬件一致）
  - 验证 Ed25519 签名
 */
bool CheckFirmware::check_OTA_next(const esp_partition_t *part, const uint8_t *lead_bytes, uint32_t lead_length)
{
    Serial.printf("Running partition %s\n", esp_ota_get_running_partition()->label);

    uint32_t board_id = 0;
    bool sig_ok = check_OTA_partition(part, lead_bytes, lead_length, board_id);

    if (g.lock_level == -1) {
        // lock_level == -1：调试模式，跳过签名验证
        return true;
    }

    // 板 ID 不为零时必须与当前硬件匹配
    if (board_id != 0 && board_id != BOARD_ID) {
        return false;
    }

    return sig_ok;
}

/*
  check_OTA_running() - 验证当前运行分区的固件签名（启动时调用）
  用于确认当前运行的固件是合法签名的。
 */
bool CheckFirmware::check_OTA_running(void)
{
    const auto *running_part = esp_ota_get_running_partition();
    if (running_part == nullptr) {
        Serial.printf("No running OTA partition\n");
        return false;
    }
    uint32_t board_id=0;
    return check_OTA_partition(running_part, nullptr, 0, board_id);
}
        
esp_err_t esp_partition_read_raw(const esp_partition_t* partition,
                                 size_t src_offset, void* dst, size_t size);
    
