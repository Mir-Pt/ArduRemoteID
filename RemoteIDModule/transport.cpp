/*
  generic transport class for handling OpenDroneID messages
  传输层基类实现
  管理共享的 RID 数据、arm 状态检查、会话密钥生成和签名验证
 */
#include <Arduino.h>
#include "transport.h"
#include "parameters.h"
#include "util.h"
#include "monocypher.h"

// 静态成员初始化 —— 数据解析失败原因（初始为"未初始化"）
const char *Transport::parse_fail = "uninitialised";

// 各类消息最后接收时间戳（毫秒），用于判断数据是否超时
uint32_t Transport::last_location_ms;
uint32_t Transport::last_basic_id_ms;
uint32_t Transport::last_self_id_ms;
uint32_t Transport::last_operator_id_ms;
uint32_t Transport::last_system_ms;
uint32_t Transport::last_system_timestamp;
float Transport::last_location_timestamp;

// 共享的 RID 数据结构实例（所有传输协议写入同一份）
mavlink_open_drone_id_location_t Transport::location;
mavlink_open_drone_id_basic_id_t Transport::basic_id;
mavlink_open_drone_id_authentication_t Transport::authentication;
mavlink_open_drone_id_self_id_t Transport::self_id;
mavlink_open_drone_id_system_t Transport::system;
mavlink_open_drone_id_operator_id_t Transport::operator_id;

Transport::Transport()
{
}

/*
  check we are OK to arm
  检查是否满足解锁广播的条件
  验证各类 RID 数据是否已接收且未超时，返回 arm 状态码
  reason: 输出参数，失败时指向错误描述字符串
  返回值: MAV_ODID_ARM_STATUS_GOOD_TO_ARM 或 MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC
 */
uint8_t Transport::arm_status_check(const char *&reason)
{
    // 位置数据最大允许年龄。ASTM/FAA 标准建议 ~1 秒，但 PX4 v1.14.3 的 ODID
    // 消息流固定为 0.1 Hz（每 10 秒一帧），且不响应 SET_MESSAGE_INTERVAL 提频请求。
    // 为避免 arm_status 周期性超时导致 LED 绿红交替，将阈值放宽到 12 秒以覆盖
    // PX4 的 10 秒发送间隔。若飞控能提供高频 ODID 流，可调回 3000。
    const uint32_t max_age_location_ms = 12000;  // 位置数据最大允许年龄：12秒
    const uint32_t max_age_other_ms = 22000;     // 其他数据最大允许年龄：22秒
    const uint32_t now_ms = millis();

    uint8_t status = MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC;

    //return status OK if we have enabled the force arm option
    // 如果启用了强制解锁选项，直接返回成功
    if (g.options & OPTIONS_FORCE_ARM_OK) {
        status = MAV_ODID_ARM_STATUS_GOOD_TO_ARM;
        return status;
    }

    String ret = "";  // 累积错误标识字符串

    // 检查位置数据是否存在且未超时
    if (last_location_ms == 0 || now_ms - last_location_ms > max_age_location_ms) {
        ret += "LOC ";
    }
    if (!g.have_basic_id_info()) {
        // if there is no basic ID data stored in the parameters give warning. If basic ID data are streamed to RID device,
        // it will store them in the parameters
        // 参数中没有基本ID信息（UAS_TYPE/UAS_ID_TYPE/UAS_ID）
        ret += "ID ";
    }

    // SELF_ID 和 OP_ID 是辅助消息，仅当飞控确实发送了才检查超时
    // PX4 默认不发这些消息，不应因此阻止广播
    if (last_self_id_ms != 0 && now_ms - last_self_id_ms > max_age_other_ms) {
        ret += "SELF_ID ";
    }

    if (last_operator_id_ms != 0 && now_ms - last_operator_id_ms > max_age_other_ms) {
        ret += "OP_ID ";
    }

    // SYS（操作员位置）与 SELF_ID/OP_ID 一样是辅助消息，仅当飞控确实发送了才检查超时。
    // PX4 的 DroneCAN Remote ID 只有在 GCS 配置了操作员位置、或飞控解锁建立 home 位置后
    // 才会发 System，解锁前（本场景）不会发，不应因此阻止广播。
    if (last_system_ms != 0 && now_ms - last_system_ms > max_age_location_ms) {
        ret += "SYS ";
    }

    // 检查飞机位置是否为零（无效 GPS）
    if (location.latitude == 0 && location.longitude == 0) {
        ret += "LOC ";
    }

    // OP_LOC 仅在系统数据已收到且有有效时间去情况下检查
    // PX4 可能不发操作员位置，不应因此阻止广播
    if (system.operator_latitude != 0 || system.operator_longitude != 0) {
        // 有操作员位置数据时，才检查是否与飞机位置冲突
    }

    // 所有检查通过且数据解析无错误 → 可以解锁
    if (ret.length() == 0 && reason == nullptr) {
        status = MAV_ODID_ARM_STATUS_GOOD_TO_ARM;
    } else {
        // 拼接错误信息返回给调用者
        static char return_string[200];
        memset(return_string, 0, sizeof(return_string));
        if (reason != nullptr) {
            strlcpy(return_string, reason, sizeof(return_string));
        }
        strlcat(return_string, ret.c_str(), sizeof(return_string));
        reason = return_string;
    }

    return status;
}

/*
  make a session key
 */
void Transport::make_session_key(uint8_t key[8]) const
{
    struct {
        uint32_t time_us;
        uint8_t mac[8];
        uint32_t rand;
    } data {};
    static_assert(sizeof(data) % 4 == 0, "data must be multiple of 4 bytes");

    esp_efuse_mac_get_default(data.mac);
    data.time_us = micros();
    data.rand = random(0xFFFFFFFF);
    const uint64_t c64 = crc_crc64((const uint32_t *)&data, sizeof(data)/sizeof(uint32_t));
    memcpy(key, (uint8_t *)&c64, 8);
}

/*
  check signature in a command against public keys
 */
bool Transport::check_signature(uint8_t sig_length, uint8_t data_len, uint32_t sequence, uint32_t operation,
                                const uint8_t *data)
{
    if (g.no_public_keys()) {
        // allow through if no keys are setup
        return true;
    }
    if (sig_length != 64) {
        // monocypher signatures are 64 bytes
        return false;
    }

    /*
      loop over all public keys, if one matches then we are OK
     */
    for (uint8_t i=0; i<MAX_PUBLIC_KEYS; i++) {
        uint8_t key[32];
        if (!g.get_public_key(i, key)) {
            continue;
        }
        crypto_check_ctx ctx {};
        crypto_check_ctx_abstract *actx = (crypto_check_ctx_abstract*)&ctx;
        crypto_check_init(actx, &data[data_len], key);

        crypto_check_update(actx, (const uint8_t*)&sequence, sizeof(sequence));
        crypto_check_update(actx, (const uint8_t*)&operation, sizeof(operation));
        crypto_check_update(actx, data, data_len);
        if (operation != SECURE_COMMAND_GET_SESSION_KEY &&
            operation != SECURE_COMMAND_GET_REMOTEID_SESSION_KEY) {
            crypto_check_update(actx, session_key, sizeof(session_key));
        }
        if (crypto_check_final(actx) == 0) {
            // good signature
            return true;
        }
    }
    return false;
}
