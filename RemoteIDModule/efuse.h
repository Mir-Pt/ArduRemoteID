/*
  eFuse 安全熔丝配置

  通过烧录 ESP32 eFuse 禁用调试和下载接口，防止未授权固件刷写。
  当 lock_level >= 2 时自动烧录。
  注意：eFuse 一旦烧录不可撤销！
 */
#pragma once

void set_efuses(void);  // 检查并烧录安全相关的 eFuse 位
