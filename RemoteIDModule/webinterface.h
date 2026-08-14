/*
  Web 管理界面模块

  提供基于 WiFi 的 Web 服务器，支持：
  - 参数查看和修改
  - OTA（空中升级）固件更新

  固件更新时会先缓存前 16 字节（lead_bytes），
  用于在写入 Flash 之前验证固件签名。
 */
#pragma once

#include "options.h"
#include <Arduino.h>
#include "version.h"

// Web 管理界面类
class WebInterface {
public:
    void init(void);        // 初始化 Web 服务器（配置路由、启动 WiFi AP）
    void update(void);      // 周期更新（处理 HTTP 请求）
private:
    bool initialised = false;   // 是否已完成初始化

    // OTA 固件更新：缓存前 16 字节用于签名验证
    uint8_t lead_bytes[16];     // 固件头部缓冲区
    uint8_t lead_len;           // 已缓存的字节数
};
