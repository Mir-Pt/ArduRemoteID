/*
  parent class for transmission methods
  广播发射器基类
  WiFi_TX 和 BLE_TX 的父类，提供通用接口和工具方法
 */
#pragma once

#include <Arduino.h>
#include <opendroneid.h>

class Transmitter {
public:
    virtual bool init(void); // 初始化发射器（子类重写）

protected:
    // 生成随机 MAC 地址（6字节），用于广播时防止设备被追踪
    void generate_random_mac(uint8_t mac[6]);
};
