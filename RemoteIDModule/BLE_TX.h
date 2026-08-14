/*
  BLE 蓝牙广播发射驱动（BT4 传统模式 + BT5 远距离模式）

  BT4 Legacy 模式：使用传统蓝牙广播（ADV_NONCONN_IND），兼容所有手机，
                   但每次只能发送一条 OpenDroneID 消息（受限于 31 字节广播载荷）。
  BT5 Long Range 模式：使用 BLE 5.0 扩展广播 + 编码 PHY（Coded PHY, S8），
                       可一次发送完整消息包，传输距离更远（约为 BT4 的 4 倍）。
 */
#pragma once

#include "transmitter.h"

// BLE 蓝牙广播发射器类，继承自 Transmitter 基类
class BLE_TX : public Transmitter {
public:
    bool init(void) override;                               // 初始化 BLE 硬件和广播参数
    bool transmit_longrange(ODID_UAS_Data &UAS_data);       // BT5 远距离模式广播（编码 PHY，完整消息包）
    bool transmit_legacy(ODID_UAS_Data &UAS_data);          // BT4 传统模式广播（每次一条消息，轮流发送）

private:
    bool initialised;                                       // BLE 是否已完成初始化
    uint8_t msg_counters[ODID_MSG_COUNTER_AMOUNT];          // 各消息类型的发送计数器（用于轮流发送）
    uint8_t legacy_payload[36];                             // BT4 广播载荷缓冲区（31 字节 ADV + 头部开销）
    uint8_t longrange_payload[250];                         // BT5 扩展广播载荷缓冲区（可容纳完整消息包）
    bool started;                                           // 广播是否已启动

    uint8_t dBm_to_tx_power(float dBm) const;              // 将 dBm 功率值转换为 ESP32 BLE 发射功率等级
};
