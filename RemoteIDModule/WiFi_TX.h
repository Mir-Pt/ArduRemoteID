/*
  WiFi 广播发射驱动
  通过 WiFi NAN（邻居感知网络）和 Beacon（信标帧）两种模式
  广播 OpenDroneID 远程识别信息。

  NAN 模式：使用 WiFi Aware 协议的 Action 帧发送，接收端需支持 NAN。
  Beacon 模式：使用标准 WiFi 信标帧发送，兼容性更广，任何 WiFi 设备均可接收。
 */
#pragma once

#include "transmitter.h"

// WiFi 广播发射器类，继承自 Transmitter 基类
class WiFi_TX : public Transmitter {
public:
    bool init(void) override;                           // 初始化 WiFi 硬件和配置
    bool transmit_nan(ODID_UAS_Data &UAS_data);         // 通过 NAN Action 帧广播远程识别数据
    bool transmit_beacon(ODID_UAS_Data &UAS_data);      // 通过 Beacon 信标帧广播远程识别数据

private:
    bool initialised;                   // WiFi 是否已完成初始化
    char ssid[32];                      // Beacon 模式使用的 SSID 名称
    uint8_t WiFi_mac_addr[6];           // WiFi 适配器的 MAC 地址（6 字节）
    size_t ssid_length;                 // SSID 字符串的长度
    uint8_t send_counter_nan;           // NAN 帧序列号（每次发送递增）
    uint8_t send_counter_beacon;        // Beacon 帧序列号（每次发送递增）
    uint8_t dBm_to_tx_power(float dBm) const;  // 将 dBm 功率值转换为 ESP32 发射功率等级
};
