/*
  通用工具函数和宏定义

  提供项目中多处使用的基础工具：
  - MIN 宏：取两值中较小者
  - ARRAY_SIZE 宏：计算静态数组元素数量
  - crc_crc64：64 位 CRC 校验（用于固件签名验证）
  - base64_decode/encode：Base64 编解码（用于公钥的存储和传输）
 */
#pragma once

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))          // 取两值中较小者
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))  // 计算数组元素数量

#include <stdint.h>

// 64 位 CRC 校验（来自 ArduPilot），用于固件映像完整性验证
uint64_t crc_crc64(const uint32_t *data, uint16_t num_words);

// Base64 解码：将 Base64 字符串解码为二进制数据，返回解码后的字节数
int32_t base64_decode(const char *s, uint8_t *out, const uint32_t max_len);

// Base64 编码：将二进制数据编码为 Base64 字符串，返回动态分配的字符串（调用者需释放）
char *base64_encode(const uint8_t *buf, int len);
