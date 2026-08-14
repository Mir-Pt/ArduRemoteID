/*
  mavlink class for handling OpenDroneID messages
  MAVLink 串口传输层实现
  继承 Transport 基类，通过串口接收飞控发来的 MAVLink 消息，
  解析 OpenDroneID 数据并写入共享 RID 数据结构，同时发送心跳和 arm 状态。
 */
#pragma once
#include "transport.h"
#include "parameters.h"

/*
  abstraction for MAVLink on a serial port
  MAVLink 串口传输类
  支持双通道：UART（连接飞控）和 USB（调试/SITL 仿真）
 */
class MAVLinkSerial : public Transport {
public:
    using Transport::Transport;
    // 构造函数：绑定硬件串口和 MAVLink 通道号
    MAVLinkSerial(HardwareSerial &serial, mavlink_channel_t chan);
    void init(void) override;   // 初始化：打印版本横幅，设置系统ID
    void update(void) override; // 周期调用：处理收发和参数流式传输

private:
    HardwareSerial &serial;             // 绑定的硬件串口引用
    mavlink_channel_t chan;             // MAVLink 通道编号（COMM_0 或 COMM_1）
    uint32_t last_hb_ms;               // 上次发送心跳的时间戳
    uint32_t last_hb_warn_ms;          // 上次打印"等待心跳"警告的时间戳
    uint32_t param_request_last_ms;    // 参数列表流式发送的节流时间戳
    const Parameters::Param *param_next; // 参数列表流式发送的当前指针

    void update_receive(void);  // 从串口读取并解析 MAVLink 数据包
    void update_send(void);     // 周期发送心跳和 arm 状态
    // 处理已解析的 MAVLink 消息（分发到各 ODID 消息处理器）
    void process_packet(mavlink_status_t &status, mavlink_message_t &msg);
    // 通过 MAVLink STATUSTEXT 发送调试信息
    void mav_printf(uint8_t severity, const char *fmt, ...);
    // 处理安全命令（密钥管理、远程配置）
    void handle_secure_command(const mavlink_secure_command_t &pkt);
    // 向飞控请求 ODID 消息（BASIC_ID 等需显式请求的消息）
    void request_odid_messages(void);

    void arm_status_send(void); // 发送 arm 状态消息给飞控
};
