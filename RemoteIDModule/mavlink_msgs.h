/*
  mavlink message definitions
  MAVLink 消息定义与配置
  配置 MAVLink 库的编译选项，引入 MAVLink 头文件，声明通信接口函数
 */
#pragma once

// we have separate helpers disabled to make it possible
// to select MAVLink 1.0 in the arduino GUI build
// 禁用 MAVLink 内联辅助函数，使用独立编译的辅助函数
#define MAVLINK_SEPARATE_HELPERS
// 禁用 MAVLink 字节序转换辅助函数
#define MAVLINK_NO_CONVERSION_HELPERS

// 定义串口发送函数宏，MAVLink 库通过此宏发送数据
#define MAVLINK_SEND_UART_BYTES(chan, buf, len) comm_send_buffer(chan, buf, len)

// two buffers, one for USB, one for UART. This makes for easier testing with SITL
// 两个通信通道缓冲区：通道0用于 UART（Serial1），通道1用于 USB（Serial）
#define MAVLINK_COMM_NUM_BUFFERS 2

// MAVLink 最大载荷长度（字节）
#define MAVLINK_MAX_PAYLOAD_LEN 255

// 引入 MAVLink V2 类型定义（mavlink_types.h 等）
#include "mavlink2.h"

/// MAVLink system definition
/// MAVLink 系统标识（系统ID + 组件ID）
extern mavlink_system_t mavlink_system;

// 串口发送缓冲区函数声明（在 mavlink.cpp 中实现）
void comm_send_buffer(mavlink_channel_t chan, const uint8_t *buf, uint8_t len);

// 启用 MAVLink 便捷函数（消息打包/发送的简化接口）
#define MAVLINK_USE_CONVENIENCE_FUNCTIONS
// 引入所有 MAVLink 消息定义（包含 OpenDroneID dialect）
#include <generated/all/mavlink.h>
