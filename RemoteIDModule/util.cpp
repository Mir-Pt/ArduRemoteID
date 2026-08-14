/*
  通用工具函数实现

  提供 CRC64 校验和 Base64 编解码功能：
  - crc_crc64：64 位 CRC 校验（来自 ArduPilot），用于固件映像完整性验证
  - base64_decode：Base64 字符串解码为二进制数据（用于公钥解码）
  - base64_encode：二进制数据编码为 Base64 字符串（用于公钥存储）
 */
#include "util.h"
#include <string.h>

/*
  crc_crc64() - 64 位 CRC 校验计算
  使用多项式 0x42F0E1EBA9EA3693（ECMA-182 标准）。
  逐字节处理输入数据，每字节进行 8 次移位异或运算。
  初始值和最终值均取反（标准 CRC 处理方式）。
 */
uint64_t crc_crc64(const uint32_t *data, uint16_t num_words)
{
    const uint64_t poly = 0x42F0E1EBA9EA3693ULL;  // CRC-64 多项式
    uint64_t crc = ~(0ULL);  // 初始值全 1
    while (num_words--) {
        uint32_t value = *data++;
        for (uint8_t j = 0; j < 4; j++) {  // 逐字节处理每个 32 位字
            uint8_t byte = ((uint8_t *)&value)[j];
            crc ^= (uint64_t)byte << 56u;  // 将字节放到 CRC 最高位
            for (uint8_t i = 0; i < 8; i++) {  // 8 次移位
                if (crc & (1ull << 63u)) {
                    crc = (uint64_t)(crc << 1u) ^ poly;
                } else {
                    crc = (uint64_t)(crc << 1u);
                }
            }
        }
    }
    crc ^= ~(0ULL);  // 最终取反

    return crc;
}

// Base64 字符表
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
  base64_decode() - Base64 解码
  将 Base64 编码的字符串解码为原始二进制数据。
  实现简洁但非高效（逐字符查表），适合嵌入式环境的小数据量场景。
  处理 '=' 填充字符（末尾每个 '=' 减少 1 字节输出）。
  返回解码后的字节数，失败返回 0。
 */
int32_t base64_decode(const char *s, uint8_t *out, const uint32_t max_len)
{
    const char *p;
    uint32_t n = 0;
    uint32_t i = 0;
    while (*s && (p=strchr(b64,*s))) {
        const uint8_t idx = (p - b64);
        const uint32_t byte_offset = (i*6)/8;
        const uint32_t bit_offset = (i*6)%8;
        out[byte_offset] &= ~((1<<(8-bit_offset))-1);
        if (bit_offset < 3) {
            if (byte_offset >= max_len) {
                break;
            }
            out[byte_offset] |= (idx << (2-bit_offset));
            n = byte_offset+1;
        } else {
            if (byte_offset >= max_len) {
                break;
            }
            out[byte_offset] |= (idx >> (bit_offset-2));
            n = byte_offset+1;
            if (byte_offset+1 >= max_len) {
                break;
            }
            out[byte_offset+1] = (idx << (8-(bit_offset-2))) & 0xFF;
            n = byte_offset+2;
        }
        s++; i++;
    }

    if ((n > 0) && (*s == '=')) {
        n -= 1;
    }

    return n;
}

/*
  base64_encode() - Base64 编码
  将二进制数据编码为 Base64 字符串。
  返回动态分配的字符串（调用者需使用 delete[] 释放）。
  自动添加 '=' 填充使输出长度为 4 的倍数。
*/
char *base64_encode(const uint8_t *d, int len)
{
    uint32_t bit_offset, byte_offset, idx, i;
    uint32_t bytes = (len*8 + 5)/6;
    uint32_t pad_bytes = (bytes % 4) ? 4 - (bytes % 4) : 0;

    char *out = new char[bytes+pad_bytes+1];
    if (!out) {
        return nullptr;
    }

    for (i=0;i<bytes;i++) {
        byte_offset = (i*6)/8;
        bit_offset = (i*6)%8;
        if (bit_offset < 3) {
            idx = (d[byte_offset] >> (2-bit_offset)) & 0x3FU;
        } else {
            idx = (d[byte_offset] << (bit_offset-2)) & 0x3FU;
            if (byte_offset+1 < len) {
                idx |= (d[byte_offset+1] >> (8-(bit_offset-2)));
            }
        }
        out[i] = b64[idx];
    }

    for (;i<bytes+pad_bytes;i++) {
        out[i] = '=';
    }
    out[i] = 0;

    return out;
}
