/*
  control optional behaviour in the firmware at build time
  编译时功能开关配置
  通过宏定义控制固件中哪些功能模块被编译进去
 */
#pragma once

#include "board_config.h"

// do we support DroneCAN connnection to flight controller?
// 是否启用 DroneCAN 连接飞控（取决于板级配置中是否定义了 CAN 引脚）
#define AP_DRONECAN_ENABLED defined(PIN_CAN_TX) && defined(PIN_CAN_RX)

// do we support MAVLink connnection to flight controller?
// 是否启用 MAVLink 连接飞控（默认始终启用）
#define AP_MAVLINK_ENABLED 1
