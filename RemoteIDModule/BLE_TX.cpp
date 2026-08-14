/*
  BLE 蓝牙广播发射驱动实现

  本模块实现两种 BLE 广播方式发送 OpenDroneID 远程识别信息：
  1. BT4 Legacy 模式：传统蓝牙广播，每次发送一种消息类型，轮流发送所有消息
  2. BT5 Long Range 模式：扩展广播 + 编码 PHY（S8），一次发送完整消息包

  BT4 广播载荷格式（ASTM F3411 标准）：
    [长度][0x16][0xFA 0xFF][0x0D][计数器][25字节 ODID 消息]
    - 0x16 = Service Data AD Type
    - 0xFA 0xFF = OpenDroneID 16-bit UUID（小端序）
    - 0x0D = OpenDroneID 服务数据类型标识

  致谢：
  - Roel Schiphorst (BlueMark) 协助蓝牙代码开发
  - chegewara 提供 BLE5 多广播示例代码
 */

#include "BLE_TX.h"
#include "options.h"
#include <esp_system.h>

#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include "parameters.h"



// ==================== BLE 广播参数配置 ====================

// BT4 传统广播参数（广播实例 0）
// interval_min/max 配置为约 1Hz 更新率，动态修改这些字段会失败
// 更短的间隔 = 更多广播次数 = 更高功耗 + 更多射频干扰
static esp_ble_gap_ext_adv_params_t legacy_adv_params = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_NONCONN,  // 传统不可连接广播（ADV_NONCONN_IND）
    .interval_min = 192,          // 最小广播间隔（单位 0.625ms，192 = 120ms）
    .interval_max = 267,          // 最大广播间隔（单位 0.625ms，267 ≈ 167ms）
    .channel_map = ADV_CHNL_ALL,  // 使用所有三个广播信道（37, 38, 39）
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,          // 使用随机地址（防追踪）
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST, // 单播不可连接传输
    .tx_power = 0,                // 发射功率（运行时根据参数设置）
    .primary_phy = ESP_BLE_GAP_PHY_1M,    // 主 PHY：1M（传统 BLE 物理层）
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_1M,  // 辅 PHY：1M
    .sid = 0,                     // 广播集 ID
    .scan_req_notif = false,      // 不通知扫描请求
};

// BT5 远距离广播参数（广播实例 1）
static esp_ble_gap_ext_adv_params_t ext_adv_params_coded = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_NONCONN_NONSCANNABLE_UNDIRECTED, // 不可连接、不可扫描的无向广播
    .interval_min = 1200,         // 最小广播间隔（1200 × 0.625ms = 750ms）
    .interval_max = 1600,         // 最大广播间隔（1600 × 0.625ms = 1000ms）
    .channel_map = ADV_CHNL_ALL,  // 使用所有广播信道
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,          // 使用随机地址
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST, // 单播不可连接传输
    .tx_power = 0,                // 发射功率（运行时根据参数设置）
    .primary_phy = ESP_BLE_GAP_PHY_CODED,    // 主 PHY：编码 PHY（远距离模式）
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_CODED,  // 辅 PHY：编码 PHY
    .sid = 1,                     // 广播集 ID（与 Legacy 区分）
    .scan_req_notif = false,
};

/*
  dBm_to_tx_power() - 将 dBm 功率值转换为 ESP32 BLE 发射功率等级
  ESP32 BLE 支持离散的功率等级（-21, -18, -15, ..., +15, +18 dBm），
  查表找到不超过目标 dBm 的最大等级。
 */
uint8_t BLE_TX::dBm_to_tx_power(float dBm) const
{
    // ESP32 BLE 功率等级查找表（从低到高排列）
    static const struct {
        uint8_t level;  // ESP32 功率等级枚举值
        float dBm;      // 对应的 dBm 值
    } dBm_table[] = {
        { ESP_PWR_LVL_N21,-21 },   // -21 dBm
        { ESP_PWR_LVL_N18,-18 },   // -18 dBm
        { ESP_PWR_LVL_N15,-15 },   // -15 dBm
        { ESP_PWR_LVL_N12,-12 },   // -12 dBm
        { ESP_PWR_LVL_N9,  -9 },   //  -9 dBm
        { ESP_PWR_LVL_N6,  -6 },   //  -6 dBm
        { ESP_PWR_LVL_N3,  -3 },   //  -3 dBm
        { ESP_PWR_LVL_N0,   0 },   //   0 dBm
        { ESP_PWR_LVL_P3,   3 },   //  +3 dBm
        { ESP_PWR_LVL_P6,   6 },   //  +6 dBm
        { ESP_PWR_LVL_P9,   9 },   //  +9 dBm
        { ESP_PWR_LVL_P12, 12 },   // +12 dBm
        { ESP_PWR_LVL_P15, 15 },   // +15 dBm
        { ESP_PWR_LVL_P18, 18 },   // +18 dBm（最大）
    };
    for (const auto &t : dBm_table) {
        if (dBm <= t.dBm) {
            return t.level;
        }
    }
    return ESP_PWR_LVL_P18;  // 超出范围时使用最大功率
}


// BLE 多广播实例（2 个：实例 0 = BT4 Legacy，实例 1 = BT5 Long Range）
static BLEMultiAdvertising advert(2);

/*
  init() - BLE 硬件初始化
  配置 BT4 和 BT5 两个广播实例的参数（功率、间隔、MAC 地址、PHY 编码方式）。
  仅在首次调用时执行实际初始化，后续调用直接返回 true。
 */
bool BLE_TX::init(void)
{
    if (initialised) {
        return true;
    }
    initialised = true;
    BLEDevice::init("");  // 初始化 BLE 设备（空名称，名称通过广播数据设置）

    // 根据参数设置 BT4 和 BT5 的发射功率
    legacy_adv_params.tx_power = dBm_to_tx_power(g.bt4_power);
    ext_adv_params_coded.tx_power = dBm_to_tx_power(g.bt5_power);

    // 根据配置的输出速率计算广播间隔
    // BT4：每个周期需轮流发送 7 种消息，所以间隔 = 1000ms / (速率×7种) / 0.625ms
    legacy_adv_params.interval_max =  (1000/(g.bt4_rate*7))/0.625;
    legacy_adv_params.interval_min = 0.75*legacy_adv_params.interval_max;  // 最小间隔为最大间隔的 75%
    // BT5：一次发送完整消息包，间隔 = 1000ms / 速率 / 0.625ms
    ext_adv_params_coded.interval_max =  (1000/(g.bt5_rate))/0.625;
    ext_adv_params_coded.interval_min = 0.75*ext_adv_params_coded.interval_max;

    // 生成随机 MAC 地址（防止通过固定地址追踪设备）
    uint8_t mac_addr[6];
    generate_random_mac(mac_addr);

    // 设置为蓝牙随机静态地址（最高两位 = 11）
    mac_addr[0] |= 0xc0;

    // 配置广播实例 0：BT4 传统模式
    advert.setAdvertisingParams(0, &legacy_adv_params);
    advert.setInstanceAddress(0, mac_addr);
    advert.setDuration(0);  // 持续广播（不超时）

    // 配置广播实例 1：BT5 远距离模式
    advert.setAdvertisingParams(1, &ext_adv_params_coded);
    advert.setDuration(1);  // 持续广播
    advert.setInstanceAddress(1, mac_addr);

    // 设置首选编码 PHY 为 S8（每个符号 8 位编码，传输距离最远，约为 S2 的 2 倍）
    if (esp_ble_gap_set_prefered_default_phy(ESP_BLE_GAP_PHY_OPTIONS_PREF_S8_CODING, ESP_BLE_GAP_PHY_OPTIONS_PREF_S8_CODING) != ESP_OK) {
        Serial.printf("Failed to setup S8 coding\n");
    }

    // 清零所有消息类型的发送计数器
    memset(&msg_counters, 0, sizeof(msg_counters));
    return true;
}

#define IMIN(a,b) ((a)<(b)?(a):(b))  // 取两值中较小者

/*
  transmit_longrange() - BT5 远距离模式广播
  将所有有效消息打包为一个 ODID 消息包（Message Pack），
  添加 ASTM 标准帧头后通过 BLE 5.0 编码 PHY 广播发送。
  编码 PHY (S8) 以降低数据率换取更远的传输距离。
 */
bool BLE_TX::transmit_longrange(ODID_UAS_Data &UAS_data)
{
    init();

    // 构建 OpenDroneID 消息包（包含所有有效消息类型）
    uint8_t payload[250];
    int length = odid_message_build_pack(&UAS_data, payload, 255);
    if (length <= 0) {
        return false;
    }

    // 构建 ASTM F3411 BLE 帧头：
    // [总长度+5] [0x16=Service Data] [0xFA 0xFF=OpenDroneID UUID] [0x0D=类型] [计数器]
    const uint8_t header[] { uint8_t(length+5), 0x16, 0xfa, 0xff, 0x0d, uint8_t(msg_counters[ODID_MSG_COUNTER_PACKED]++) };

    // 组装完整广播数据：帧头 + 消息包载荷
    memcpy(longrange_payload, header, sizeof(header));
    memcpy(&longrange_payload[sizeof(header)], payload, length);
    int longrange_length = sizeof(header) + length;

    // 更新广播实例 1（BT5）的数据
    advert.setAdvertisingData(1, longrange_length, longrange_payload);

    // 首次有数据时启动广播（BT4 和 BT5 同时启动）
    if (!started) {
        advert.start();
    }
    started = true;

    return true;
}

/*
  transmit_legacy() - BT4 传统模式广播
  由于 BT4 广播载荷限制（最大 31 字节），每次只能发送一种消息类型。
  通过 legacy_phase 轮流发送各类消息，发送顺序为：
    0: Location（位置）
    1: BasicID[0]（基本标识 1）
    2: SelfID（自我标识）
    3: System（系统信息）
    4: OperatorID（操作员标识）
    5: BasicID[1]（基本标识 2，仅双 ID 时）
    6: BLE 设备名称（ArduRemoteID_XXXX）
 */
bool BLE_TX::transmit_legacy(ODID_UAS_Data &UAS_data)
{
    init();
    static uint8_t legacy_phase = 0;  // 当前发送阶段（轮流计数器）
    int legacy_length = 0;

    // ASTM F3411 BLE 帧头（不含消息计数器）：
    // [0x1E=总长30字节] [0x16=Service Data] [0xFA 0xFF=OpenDroneID UUID] [0x0D=类型]
    const uint8_t header[] { 0x1e, 0x16, 0xfa, 0xff, 0x0d };

    // 清空载荷缓冲区并写入帧头
    memset(legacy_payload, 0, sizeof(legacy_payload));
    memcpy(legacy_payload, header, sizeof(header));
    legacy_length = sizeof(header);

    switch (legacy_phase)
    {
    case  0: {
        // 阶段 0：发送位置消息（Location）
        if (UAS_data.LocationValid) {
            ODID_Location_encoded location_encoded;
            memset(&location_encoded, 0, sizeof(location_encoded));
            if (encodeLocationMessage(&location_encoded, &UAS_data.Location) != ODID_SUCCESS) {
                break;
            }

            // 写入消息计数器（帧头之后 1 字节）并递增
            memcpy(&legacy_payload[sizeof(header)], &msg_counters[ODID_MSG_COUNTER_LOCATION], 1);
            msg_counters[ODID_MSG_COUNTER_LOCATION]++;

            // 写入编码后的位置消息数据
            memcpy(&legacy_payload[sizeof(header) + 1], &location_encoded, sizeof(location_encoded));
            legacy_length = sizeof(header) + 1 + sizeof(location_encoded);
        }
        break;
    }

    case  1: {
        // 阶段 1：发送基本标识消息 1（BasicID[0]）
        if (UAS_data.BasicIDValid[0]) {
            ODID_BasicID_encoded basicid_encoded;
            memset(&basicid_encoded, 0, sizeof(basicid_encoded));
            if (encodeBasicIDMessage(&basicid_encoded, &UAS_data.BasicID[0]) != ODID_SUCCESS) {
                break;
            }

            memcpy(&legacy_payload[sizeof(header)], &msg_counters[ODID_MSG_COUNTER_BASIC_ID], 1);
            msg_counters[ODID_MSG_COUNTER_BASIC_ID]++;

            memcpy(&legacy_payload[sizeof(header) + 1], &basicid_encoded, sizeof(basicid_encoded));
            legacy_length = sizeof(header) + 1 + sizeof(basicid_encoded);
        }
        break;
    }

    case  2: {
        // 阶段 2：发送自我标识消息（SelfID）
        if (UAS_data.SelfIDValid) {
            ODID_SelfID_encoded selfid_encoded;
            memset(&selfid_encoded, 0, sizeof(selfid_encoded));
            if (encodeSelfIDMessage(&selfid_encoded, &UAS_data.SelfID) != ODID_SUCCESS) {
                break;
            }

            memcpy(&legacy_payload[sizeof(header)], &msg_counters[ODID_MSG_COUNTER_SELF_ID], 1);
            msg_counters[ODID_MSG_COUNTER_SELF_ID]++;

            memcpy(&legacy_payload[sizeof(header) + 1], &selfid_encoded, sizeof(selfid_encoded));
            legacy_length = sizeof(header) + 1 + sizeof(selfid_encoded);
        }
        break;
    }

    case  3: {
        // 阶段 3：发送系统消息（System）
        if (UAS_data.SystemValid) {
            ODID_System_encoded system_encoded;
            memset(&system_encoded, 0, sizeof(system_encoded));
            if (encodeSystemMessage(&system_encoded, &UAS_data.System) != ODID_SUCCESS) {
                break;
            }

            memcpy(&legacy_payload[sizeof(header)], &msg_counters[ODID_MSG_COUNTER_SYSTEM], 1);
            msg_counters[ODID_MSG_COUNTER_SYSTEM]++;

            memcpy(&legacy_payload[sizeof(header) + 1], &system_encoded, sizeof(system_encoded));
            legacy_length = sizeof(header) + 1 + sizeof(system_encoded);
        }
        break;
    }

    case  4: {
        // 阶段 4：发送操作员标识消息（OperatorID）
        if (UAS_data.OperatorIDValid) {
            ODID_OperatorID_encoded operatorid_encoded;
            memset(&operatorid_encoded, 0, sizeof(operatorid_encoded));
            if (encodeOperatorIDMessage(&operatorid_encoded, &UAS_data.OperatorID) != ODID_SUCCESS) {
                break;
            }

            memcpy(&legacy_payload[sizeof(header)], &msg_counters[ODID_MSG_COUNTER_OPERATOR_ID], 1);
            msg_counters[ODID_MSG_COUNTER_OPERATOR_ID]++;

            memcpy(&legacy_payload[sizeof(header) + 1], &operatorid_encoded, sizeof(operatorid_encoded));
            legacy_length = sizeof(header) + 1 + sizeof(operatorid_encoded);
        }
        break;
    }

    case  5:
        // 阶段 5：发送基本标识消息 2（BasicID[1]，仅双 ID 模式时有效）
        if (UAS_data.BasicIDValid[1]) {
            ODID_BasicID_encoded basicid2_encoded;
            memset(&basicid2_encoded, 0, sizeof(basicid2_encoded));
            if (encodeBasicIDMessage(&basicid2_encoded, &UAS_data.BasicID[1]) != ODID_SUCCESS) {
                break;
            }

            memcpy(&legacy_payload[sizeof(header)], &msg_counters[ODID_MSG_COUNTER_BASIC_ID], 1);
            msg_counters[ODID_MSG_COUNTER_BASIC_ID]++;

            memcpy(&legacy_payload[sizeof(header) + 1], &basicid2_encoded, sizeof(basicid2_encoded));
            legacy_length = sizeof(header) + 1 + sizeof(basicid2_encoded);
        }
        break;

    case  6: {
        // 阶段 6：发送 BLE 设备名称广播
        // 名称格式："ArduRemoteID_XXXX"（XXXX 为 UAS_ID 末尾最多 4 字符）
        char legacy_name[28] {};
        const char *UAS_ID = (const char *)UAS_data.BasicID[0].UASID;
        const uint8_t ID_len = strlen(UAS_ID);
        const uint8_t ID_tail = IMIN(4, ID_len);  // 取 ID 末尾最多 4 个字符
        snprintf(legacy_name, sizeof(legacy_name), "ArduRemoteID_%s", &UAS_ID[ID_len-ID_tail]);

        // 构建 BLE 名称广播数据：[Flags AD] [Name AD]
        memset(legacy_payload, 0, sizeof(legacy_payload));
        const uint8_t legacy_name_header[] { 0x02, 0x01, 0x06, uint8_t(strlen(legacy_name)+1), ESP_BLE_AD_TYPE_NAME_SHORT};
        //                                   Flags 长度  类型  通用可发现+不支持BR/EDR  名称长度+1            短名称类型

        memcpy(legacy_payload, legacy_name_header, sizeof(legacy_name_header));
        memcpy(&legacy_payload[sizeof(legacy_name_header)], legacy_name, strlen(legacy_name) + 1);

        legacy_length = sizeof(legacy_name_header) + strlen(legacy_name) + 1; // +1 包含 null 终止符
        break;
    }
    }

    // 推进到下一个发送阶段（循环）
    legacy_phase++;

    if (UAS_data.BasicIDValid[1]) {
        legacy_phase %= 7;  // 有双 ID 时循环 0-6（7 个阶段）
    } else {
        legacy_phase %= 6;  // 单 ID 时循环 0-5（跳过阶段 5，6 个阶段）
    }

    // 更新广播实例 0（BT4）的数据
    advert.setAdvertisingData(0, legacy_length, legacy_payload);

    // 首次有数据时启动广播
    if (!started) {
        advert.start();
    }
    started = true;

    return true;
}
