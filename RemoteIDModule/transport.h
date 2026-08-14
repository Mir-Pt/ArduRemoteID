/*
  parent class for handling transports
  传输层基类
  所有传输协议（MAVLink、DroneCAN）的抽象父类，
  持有静态共享的 RID 数据，无论从哪个协议收到数据都写入同一份。
 */
#pragma once

#include "mavlink_msgs.h"

/*
  abstraction for opendroneid transports
  OpenDroneID 传输层抽象类
 */
class Transport {
public:
    Transport();
    virtual void init(void) = 0;   // 初始化（子类实现）
    virtual void update(void) = 0; // 周期更新（子类实现）
    // 检查是否满足解锁广播条件，返回 MAVLink arm_status，reason 输出失败原因
    uint8_t arm_status_check(const char *&reason);

    // 以下 getter 方法提供对共享 RID 数据的只读访问
    const mavlink_open_drone_id_location_t &get_location(void) const {
        return location;
    }

    const mavlink_open_drone_id_basic_id_t &get_basic_id(void) const {
        return basic_id;
    }

    const mavlink_open_drone_id_authentication_t &get_authentication(void) const {
        return authentication;
    }

    const mavlink_open_drone_id_self_id_t &get_self_id(void) const {
        return self_id;
    }

    const mavlink_open_drone_id_system_t &get_system(void) const {
        return system;
    }

    const mavlink_open_drone_id_operator_id_t &get_operator_id(void) const {
        return operator_id;
    }

    // 获取最后一次收到 location 消息的时间戳（毫秒）
    uint32_t get_last_location_ms(void) const {
        return last_location_ms;
    }

    // 获取最后一次收到 system 消息的时间戳（毫秒）
    uint32_t get_last_system_ms(void) const {
        return last_system_ms;
    }

    // 设置数据解析失败的错误信息
    void set_parse_fail(const char *msg) {
        parse_fail = msg;
    }

    // 获取数据解析失败的错误信息（nullptr 表示无错误）
    const char *get_parse_fail(void) {
        return parse_fail;
    }

protected:
    // common variables between transports. The last message of each
    // type, no matter what transport it was on, wins
    // 所有传输协议共享的静态变量，最后收到的消息覆盖之前的（无论来自哪个协议）
    static const char *parse_fail;  // 数据解析失败原因

    static uint32_t last_location_ms;       // 最后收到位置消息的时间
    static uint32_t last_basic_id_ms;       // 最后收到基本ID消息的时间
    static uint32_t last_self_id_ms;        // 最后收到自述ID消息的时间
    static uint32_t last_operator_id_ms;    // 最后收到操作员ID消息的时间
    static uint32_t last_system_ms;         // 最后收到系统消息的时间
    static uint32_t last_system_timestamp;  // 系统消息中的时间戳
    static float last_location_timestamp;   // 位置消息中的时间戳

    // 共享的 RID 数据结构（MAVLink 格式）
    static mavlink_open_drone_id_location_t location;           // 位置信息
    static mavlink_open_drone_id_basic_id_t basic_id;           // 无人机基本ID
    static mavlink_open_drone_id_authentication_t authentication; // 认证信息
    static mavlink_open_drone_id_self_id_t self_id;             // 自述ID
    static mavlink_open_drone_id_system_t system;               // 系统信息（操作员位置等）
    static mavlink_open_drone_id_operator_id_t operator_id;     // 操作员ID

    // 生成会话密钥（用于安全命令的防重放）
    void make_session_key(uint8_t key[8]) const;

    /*
      check signature in a command against public keys
      验证命令中的 Ed25519 签名是否与存储的公钥匹配
    */
    bool check_signature(uint8_t sig_length, uint8_t data_len, uint32_t sequence, uint32_t operation,
                         const uint8_t *data);

    uint8_t session_key[8]; // 当前会话密钥（8字节，由 MAC + 时间 + 随机数生成）
};
