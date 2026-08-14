/*
  参数管理系统 - 实现文件

  本文件实现 ESP32 Remote ID 模块的参数管理功能，包括：
  1. 参数描述符表 params[]：定义所有可配置参数的名称、类型、范围和默认值
  2. 参数的 NVS（非易失性存储）读写操作：参数持久化到 ESP32 Flash
  3. 参数查找接口：支持按名称、索引查找，支持 MAVLink 浮点参数协议
  4. 参数类型转换：各类型与浮点数之间的双向转换
  5. 公钥管理：Ed25519 公钥的 Base64 编解码存储
  6. 初始化流程：加载默认值 -> 从 NVS 恢复 -> 首次初始化公钥

  参数通过 MAVLink PARAM_SET 或 DroneCAN GetSet 协议远程修改，
  修改后立即写入 NVS 实现掉电保存。
*/

#include "options.h"
#include <Arduino.h>
#include "parameters.h"
#include <nvs_flash.h>
#include <string.h>
#include "romfs.h"
#include "util.h"

Parameters g;                   // 全局参数实例，存放所有运行时参数值
static nvs_handle handle;      // NVS 存储句柄，用于所有 NVS 读写操作

/*
  ==================== 参数描述符表 ====================
  每个条目格式：{ 名称, 类型, 指针, 默认值, 最小值, 最大值, 标志位, 最小长度 }
  此表定义了模块所有可配置参数，参数通过 MAVLink/DroneCAN 协议远程读写。
  表末尾以空名称 + NONE 类型标记结束。
*/
const Parameters::Param Parameters::params[] = {
    // ---------- 安全与总线配置 ----------
    { "LOCK_LEVEL",        Parameters::ParamType::INT8,  (const void*)&g.lock_level,       0, -1, 2 },    // 参数锁定等级：-1=接受任意固件, 0=未锁定, 1=锁定, 2=eFuse烧录锁定
    { "CAN_NODE",          Parameters::ParamType::UINT8,  (const void*)&g.can_node,         0, 0, 127 },  // DroneCAN 节点 ID（0=动态分配, 1-127=固定地址）
#if defined(PIN_CAN_TERM)
    { "CAN_TERMINATE",     Parameters::ParamType::UINT8,  (const void*)&g.can_term,        0, 0, 1 },    // CAN 总线终端电阻使能（仅支持该引脚的板卡可见）
#endif

    // ---------- 无人机身份标识（BasicID 1） ----------
    { "UAS_TYPE",          Parameters::ParamType::UINT8,  (const void*)&g.ua_type,          0, 0, 15 },   // 无人机类型（ODID_UA_TYPE 枚举值）
    { "UAS_ID_TYPE",       Parameters::ParamType::UINT8,  (const void*)&g.id_type,          0, 0, 4 },    // ID 类型（序列号/注册号/UTM分配等）
    { "UAS_ID",            Parameters::ParamType::CHAR20, (const void*)&g.uas_id[0],        0, 0, 0 },    // UAS 标识字符串（最长20字符）

    // ---------- 无人机身份标识（BasicID 2，可选的第二标识） ----------
    { "UAS_TYPE_2",        Parameters::ParamType::UINT8,  (const void*)&g.ua_type_2,          0, 0, 15 }, // 第二 BasicID 的无人机类型
    { "UAS_ID_TYPE_2",     Parameters::ParamType::UINT8,  (const void*)&g.id_type_2,          0, 0, 4 },  // 第二 BasicID 的 ID 类型
    { "UAS_ID_2",          Parameters::ParamType::CHAR20, (const void*)&g.uas_id_2[0],        0, 0, 0 },  // 第二 BasicID 的标识字符串

    // ---------- 串口通信 ----------
    { "BAUDRATE",          Parameters::ParamType::UINT32, (const void*)&g.baudrate,         57600, 9600, 921600 }, // MAVLink 串口波特率（默认57600，与 PX4 TELEM 默认波特率一致）

    // ---------- WiFi 广播配置 ----------
    { "WIFI_NAN_RATE",     Parameters::ParamType::FLOAT,  (const void*)&g.wifi_nan_rate,    0, 0, 5 },    // WiFi NAN（邻域感知网络）广播速率（Hz, 0=禁用）
    { "WIFI_BCN_RATE",     Parameters::ParamType::FLOAT,  (const void*)&g.wifi_beacon_rate,    3, 0, 5 }, // WiFi Beacon 帧广播速率（Hz, 0=禁用）
    { "WIFI_POWER",        Parameters::ParamType::FLOAT,  (const void*)&g.wifi_power,       20, 2, 20 },  // WiFi 发射功率（dBm, 范围2-20）

    // ---------- 蓝牙广播配置 ----------
    { "BT4_RATE",          Parameters::ParamType::FLOAT,  (const void*)&g.bt4_rate,         1, 0, 5 },    // BT4 Legacy 广播速率（Hz, 0=禁用）
    { "BT4_POWER",         Parameters::ParamType::FLOAT,  (const void*)&g.bt4_power,        18, -27, 18 },// BT4 发射功率（dBm, 范围-27至18）
    { "BT5_RATE",          Parameters::ParamType::FLOAT,  (const void*)&g.bt5_rate,         1, 0, 5 },    // BT5 Long Range 广播速率（Hz, 0=禁用）
    { "BT5_POWER",         Parameters::ParamType::FLOAT,  (const void*)&g.bt5_power,        18, -27, 18 },// BT5 发射功率（dBm, 范围-27至18）

    // ---------- Web 服务器与 WiFi 热点配置 ----------
    { "WEBSERVER_EN",      Parameters::ParamType::UINT8,  (const void*)&g.webserver_enable, 1, 0, 1 },    // Web 管理界面使能（1=启用, 0=禁用）
    { "WIFI_SSID",         Parameters::ParamType::CHAR20, (const void*)&g.wifi_ssid, },                   // WiFi 热点 SSID（空则自动生成 RID_XXXX 格式）
    { "WIFI_PASSWORD",     Parameters::ParamType::CHAR20, (const void*)&g.wifi_password,    0, 0, 0, PARAM_FLAG_PASSWORD, 8 }, // WiFi 热点密码（最少8字符, 读取时返回掩码）
    { "WIFI_CHANNEL",      Parameters::ParamType::UINT8,  (const void*)&g.wifi_channel,    6, 1, 13 },    // WiFi 信道（1-13, 默认6）

    // ---------- 广播行为 ----------
    { "BCAST_POWERUP",     Parameters::ParamType::UINT8,  (const void*)&g.bcast_powerup,    1, 0, 1 },    // 上电后是否立即开始 Remote ID 广播

    // ---------- Ed25519 公钥（用于安全签名验证） ----------
    { "PUBLIC_KEY1",       Parameters::ParamType::CHAR64, (const void*)&g.public_keys[0], },               // 公钥槽1（Base64编码, "PUBLIC_KEYV1:" 前缀）
    { "PUBLIC_KEY2",       Parameters::ParamType::CHAR64, (const void*)&g.public_keys[1], },               // 公钥槽2
    { "PUBLIC_KEY3",       Parameters::ParamType::CHAR64, (const void*)&g.public_keys[2], },               // 公钥槽3
    { "PUBLIC_KEY4",       Parameters::ParamType::CHAR64, (const void*)&g.public_keys[3], },               // 公钥槽4
    { "PUBLIC_KEY5",       Parameters::ParamType::CHAR64, (const void*)&g.public_keys[4], },               // 公钥槽5

    // ---------- MAVLink 与系统选项 ----------
    { "MAVLINK_SYSID",     Parameters::ParamType::UINT8,  (const void*)&g.mavlink_sysid,    0, 0, 254 },  // MAVLink 系统 ID（0=从飞控心跳自动获取）
    { "OPTIONS",           Parameters::ParamType::UINT8,  (const void*)&g.options,          0, 0, 254 },  // 选项位掩码（见 OPTIONS_* 宏定义）

    // ---------- 系统控制（恢复出厂与初始化标志） ----------
    { "TO_DEFAULTS",     Parameters::ParamType::UINT8,  (const void*)&g.to_factory_defaults,    0, 0, 1 }, // 恢复出厂设置：置1后擦除NVS并重启
    { "DONE_INIT",         Parameters::ParamType::UINT8,  (const void*)&g.done_init,        0, 0, 0, PARAM_FLAG_HIDDEN}, // 首次初始化完成标志（隐藏参数, 用于判断是否需要加载默认公钥）

    // ---------- 参数表结束标记 ----------
    { "",                  Parameters::ParamType::NONE,   nullptr,  },
};

/*
  获取可表示为浮点数的参数总数

  MAVLink 参数协议要求所有参数以浮点数传输，因此只统计
  UINT8、INT8、UINT32、FLOAT 四种可转换为 float 的类型，
  排除 CHAR20/CHAR64 字符串类型和隐藏参数。
  结果减1是因为末尾的空结束标记也被统计在内。
 */
uint16_t Parameters::param_count_float(void)
{
    uint16_t count = 0;
    for (const auto &p : params) {
        // 跳过隐藏参数（如 DONE_INIT），不向外部暴露
        if (p.flags & PARAM_FLAG_HIDDEN) {
            continue;
        }
        // 仅统计可转换为 float 的数值类型
        switch (p.ptype) {
        case ParamType::UINT8:
        case ParamType::INT8:
        case ParamType::UINT32:
        case ParamType::FLOAT:
            count++;
            break;
        }
    }
    // 减1是因为参数表末尾的 NONE 空条目也被 range-for 遍历到
    return count-1;
}

/*
  获取指定参数在"浮点兼容参数列表"中的索引

  MAVLink PARAM_VALUE 消息需要 param_index 字段，
  此函数计算某个参数在仅包含数值类型参数的子集中的位置。
  如果参数不在浮点兼容列表中（如 CHAR20/CHAR64），返回 -1。
 */
int16_t Parameters::param_index_float(const Parameters::Param *f)
{
    uint16_t count = 0;
    for (const auto &p : params) {
        // 跳过隐藏参数
        if (p.flags & PARAM_FLAG_HIDDEN) {
            continue;
        }
        // 仅遍历可转换为 float 的数值类型
        switch (p.ptype) {
        case ParamType::UINT8:
        case ParamType::INT8:
        case ParamType::UINT32:
        case ParamType::FLOAT:
            // 找到目标参数，返回其在浮点列表中的位置
            if (&p == f) {
                return count;
            }
            count++;
            break;
        }
    }
    return -1; // 未找到，参数不属于浮点兼容类型
}

/*
  按名称查找参数

  遍历参数表，通过字符串比较找到匹配的参数描述符。
  用于 MAVLink PARAM_SET 和 DroneCAN GetSet 请求处理。
  返回 nullptr 表示未找到该名称的参数。
 */
const Parameters::Param *Parameters::find(const char *name)
{
    for (const auto &p : params) {
        if (strcmp(name, p.name) == 0) {
            return &p;
        }
    }
    return nullptr;
}

/*
  按绝对索引查找参数（包含所有类型）

  直接通过数组下标访问参数表，索引范围检查防止越界。
  注意：此索引包含所有参数类型（含 CHAR20/CHAR64），
  与 find_by_index_float 的浮点子集索引不同。
 */
const Parameters::Param *Parameters::find_by_index(uint16_t index)
{
    // ARRAY_SIZE(params)-2 是因为最后一个是空结束标记
    if (index >= ARRAY_SIZE(params)-2) {
        return nullptr;
    }
    return &params[index];
}

/*
  按浮点兼容索引查找参数

  仅在 UINT8/INT8/UINT32/FLOAT 类型参数中按序号查找，
  跳过隐藏参数和字符串类型参数。
  用于 MAVLink PARAM_REQUEST_READ 按 param_index 请求。
 */
const Parameters::Param *Parameters::find_by_index_float(uint16_t index)
{
    uint16_t count = 0;
    for (const auto &p : params) {
        // 跳过隐藏参数
        if (p.flags & PARAM_FLAG_HIDDEN) {
            continue;
        }
        // 仅计数数值类型参数
        switch (p.ptype) {
        case ParamType::UINT8:
        case ParamType::INT8:
        case ParamType::UINT32:
        case ParamType::FLOAT:
            if (index == count) {
                return &p;
            }
            count++;
            break;
        }
    }
    return nullptr;
}

/*
  设置 UINT8 类型参数值并写入 NVS

  特殊处理：如果参数名为 "TO_DEFAULTS" 且值为1，
  则擦除整个 NVS Flash 并立即重启 ESP32，实现恢复出厂设置。
 */
void Parameters::Param::set_uint8(uint8_t v) const
{
    auto *p = (uint8_t *)ptr;
    *p = v;
    nvs_set_u8(handle, name, *p);
    // TO_DEFAULTS 特殊逻辑：置1时擦除所有参数并重启
    if (strcmp(name, "TO_DEFAULTS") == 0) {
        if (v == 1) {
            nvs_flash_erase();  // 擦除整个 NVS 分区
            esp_restart();      // 硬件重启，重启后参数将恢复为代码中的默认值
        }
    }
}

/*
  设置 INT8 类型参数值并写入 NVS
 */
void Parameters::Param::set_int8(int8_t v) const
{
    auto *p = (int8_t *)ptr;
    *p = v;
    nvs_set_i8(handle, name, *p);
}

/*
  设置 UINT32 类型参数值并写入 NVS
 */
void Parameters::Param::set_uint32(uint32_t v) const
{
    auto *p = (uint32_t *)ptr;
    *p = v;
    nvs_set_u32(handle, name, *p);
}

/*
  设置 FLOAT 类型参数值并写入 NVS

  NVS 不直接支持 float 类型，通过 union 将 float 转为 uint32
  以二进制形式存储（利用 IEEE 754 浮点数的32位表示）。
 */
void Parameters::Param::set_float(float v) const
{
    auto *p = (float *)ptr;
    *p = v;
    // 使用 union 实现 float 到 uint32 的位级转换（非类型转换）
    union {
        float f;
        uint32_t u32;
    } u;
    u.f = v;
    nvs_set_u32(handle, name, u.u32); // 以 uint32 形式存储 float 的二进制表示
}

/*
  设置 CHAR20 类型参数值（20字符字符串）并写入 NVS

  如果参数定义了最小长度（如密码要求至少8字符），
  则输入字符串长度不足时拒绝设置。
 */
void Parameters::Param::set_char20(const char *v) const
{
    // 检查最小长度要求（如 WIFI_PASSWORD 要求至少8字符）
    if (min_len > 0 && strlen(v) < min_len) {
        return;
    }
    memset((void*)ptr, 0, 21);       // 先清零整个缓冲区（含终止符）
    strncpy((char *)ptr, v, 20);     // 复制最多20字符
    nvs_set_str(handle, name, v);    // 写入 NVS
}

/*
  设置 CHAR64 类型参数值（64字符字符串）并写入 NVS

  主要用于存储 Base64 编码的公钥字符串。
 */
void Parameters::Param::set_char64(const char *v) const
{
    // 检查最小长度要求
    if (min_len > 0 && strlen(v) < min_len) {
        return;
    }
    memset((void*)ptr, 0, 65);       // 先清零整个缓冲区（含终止符）
    strncpy((char *)ptr, v, 64);     // 复制最多64字符
    nvs_set_str(handle, name, v);    // 写入 NVS
}

/* 获取 UINT8 类型参数的当前值 */
uint8_t Parameters::Param::get_uint8() const
{
    const auto *p = (const uint8_t *)ptr;
    return *p;
}

/* 获取 INT8 类型参数的当前值 */
int8_t Parameters::Param::get_int8() const
{
    const auto *p = (const int8_t *)ptr;
    return *p;
}

/* 获取 UINT32 类型参数的当前值 */
uint32_t Parameters::Param::get_uint32() const
{
    const auto *p = (const uint32_t *)ptr;
    return *p;
}

/* 获取 FLOAT 类型参数的当前值 */
float Parameters::Param::get_float() const
{
    const auto *p = (const float *)ptr;
    return *p;
}

/* 获取 CHAR20 类型参数的字符串指针 */
const char *Parameters::Param::get_char20() const
{
    const char *p = (const char *)ptr;
    return p;
}

/* 获取 CHAR64 类型参数的字符串指针 */
const char *Parameters::Param::get_char64() const
{
    const char *p = (const char *)ptr;
    return p;
}

/*
  将参数值转换为浮点数输出

  MAVLink 参数协议要求所有参数以 float 传输，
  此函数根据参数实际类型进行适当的类型转换。
  字符串类型（CHAR20/CHAR64）无法转换，返回 false。
 */
bool Parameters::Param::get_as_float(float &v) const
{
    switch (ptype) {
        case ParamType::UINT8:
            v = float(get_uint8());
            break;
        case ParamType::INT8:
            v = float(get_int8());
            break;
        case ParamType::UINT32:
            v = float(get_uint32());
            break;
        case ParamType::FLOAT:
            v = get_float();
            break;
    default:
        return false; // CHAR20/CHAR64/NONE 类型无法转换为 float
    }
    return true;
}

/*
  从浮点数设置参数值

  MAVLink PARAM_SET 消息传入 float 值，
  此函数根据参数实际类型进行截断/转换后写入。
  注意：float 转 uint8/int8/uint32 会丢失精度。
 */
void Parameters::Param::set_as_float(float v) const
{
    switch (ptype) {
        case ParamType::UINT8:
            set_uint8(uint8_t(v));
            break;
        case ParamType::INT8:
            set_int8(int8_t(v));
            break;
        case ParamType::UINT32:
            set_uint32(uint32_t(v));
            break;
        case ParamType::FLOAT:
            set_float(v);
            break;
    }
}


/*
  加载参数默认值

  遍历参数表，将每个数值类型参数设为其 default_value 字段定义的值。
  字符串类型（CHAR20/CHAR64）的默认值由 C++ 成员初始化器提供，
  此处不处理。此函数在 init() 开头调用，确保 NVS 读取前有合理初始值。
 */
void Parameters::load_defaults(void)
{
    for (const auto &p : params) {
        switch (p.ptype) {
        case ParamType::UINT8:
            *(uint8_t *)p.ptr = uint8_t(p.default_value);
            break;
        case ParamType::INT8:
            *(int8_t *)p.ptr = int8_t(p.default_value);
            break;
        case ParamType::UINT32:
            *(uint32_t *)p.ptr = uint32_t(p.default_value);
            break;
        case ParamType::FLOAT:
            *(float *)p.ptr = p.default_value;
            break;
        }
    }
}

/*
  参数系统初始化

  完整初始化流程：
  1. 加载代码中定义的默认值
  2. 初始化 NVS Flash 并打开 "storage" 命名空间
  3. 从 NVS 读取已保存的参数值（覆盖默认值）
  4. 如果 wifi_ssid 为空，使用 MAC 地址生成唯一 SSID
  5. 检查 TO_DEFAULTS 标志，如仍为1则擦除 NVS 并重启
  6. 首次初始化时从 ROMFS 加载默认公钥
 */
void Parameters::init(void)
{
    // 第一步：加载代码中的默认值作为基础
    load_defaults();

    // 第二步：初始化 NVS Flash 子系统并打开存储命名空间
    if (nvs_flash_init() != ESP_OK ||
        nvs_open("storage", NVS_READWRITE, &handle) != ESP_OK) {
        Serial.printf("NVS init failed\n");
    }

    // 第三步：从 NVS 读取已保存的参数值，覆盖默认值
    // 如果 NVS 中没有某参数的记录，则保持默认值不变
    for (const auto &p : params) {
        switch (p.ptype) {
        case ParamType::UINT8:
            nvs_get_u8(handle, p.name, (uint8_t *)p.ptr);
            break;
        case ParamType::INT8:
            nvs_get_i8(handle, p.name, (int8_t *)p.ptr);
            break;
        case ParamType::UINT32:
            nvs_get_u32(handle, p.name, (uint32_t *)p.ptr);
            break;
        case ParamType::FLOAT:
            // float 以 uint32 二进制形式存储在 NVS 中
            nvs_get_u32(handle, p.name, (uint32_t *)p.ptr);
            break;
        case ParamType::CHAR20: {
            size_t len = 21;
            nvs_get_str(handle, p.name, (char *)p.ptr, &len);
            break;
        }
        case ParamType::CHAR64: {
            size_t len = 65;
            nvs_get_str(handle, p.name, (char *)p.ptr, &len);
            break;
        }
        }
    }

    // 第四步：如果 WiFi SSID 为空，使用 MAC 地址自动生成唯一名称
    // 格式为 "RID_XXXXXXXXXXXX"（12位十六进制 MAC 地址）
    if (strlen(g.wifi_ssid) == 0) {
        uint8_t mac[6] {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(wifi_ssid, 20, "RID_%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    }

    // 第五步：防护性检查 - 如果 TO_DEFAULTS 仍为1（不应发生），强制擦除并重启
    if (g.to_factory_defaults == 1) {
        //should not happen, but in case the parameter is still set to 1, erase flash and reboot
        nvs_flash_erase();
        esp_restart();
    }

    // 第六步：首次初始化 - 从 ROMFS 加载默认公钥
    // DONE_INIT=0 表示这是全新设备或刚恢复出厂设置
    if (g.done_init == 0) {
        set_by_name_uint8("DONE_INIT", 1); // 标记初始化已完成，下次启动不再执行

        // 加载 ArduPilot 官方公钥到槽1和槽2
        set_by_name_char64("PUBLIC_KEY1", ROMFS::find_string("public_keys/ArduPilot_public_key1.dat"));
        set_by_name_char64("PUBLIC_KEY2", ROMFS::find_string("public_keys/ArduPilot_public_key2.dat"));

        // 槽3：BlueMark 板卡加载 BlueMark 公钥，其他板卡加载 ArduPilot 第三公钥
#if defined(BOARD_BLUEMARK_DB200) || defined(BOARD_BLUEMARK_DB110) || defined(BOARD_BLUEMARK_DB202) || defined(BOARD_BLUEMARK_DB210) || defined(BOARD_BLUEMARK_DB203)
        set_by_name_char64("PUBLIC_KEY3", ROMFS::find_string("public_keys/BlueMark_public_key1.dat"));
#elif defined(BOARD_CUAV_RID)
        set_by_name_char64("PUBLIC_KEY3", ROMFS::find_string("public_keys/CUAV_public_key1.dat"));
#else
        set_by_name_char64("PUBLIC_KEY3", ROMFS::find_string("public_keys/ArduPilot_public_key3.dat"));
#endif

    }

    // 如果 BasicID 信息不完整，补全默认值（以便在没有飞控 BASIC_ID 时也能工作）
    if (!have_basic_id_info()) {
        if (g.ua_type == 0) {
            set_by_name_uint8("UAS_TYPE", 2);     // HELICOPTER_OR_MULTIROTOR
        }
        if (g.id_type == 0) {
            set_by_name_uint8("UAS_ID_TYPE", 1);  // SERIAL_NUMBER
        }
        if (strlen(g.uas_id) == 0) {
            // 用 MAC 地址生成唯一的默认 UAS_ID
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char default_id[21];
            snprintf(default_id, sizeof(default_id), "ESP32-%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            set_by_name_string("UAS_ID", default_id);
        }
    }
}

/*
  检查 BasicID 1 是否已完整配置

  Remote ID 广播需要有效的 UAS 标识信息，
  此函数验证 uas_id 非空、id_type 和 ua_type 均已设置。
  用于判断是否可以开始广播 BasicID 消息。
 */
bool Parameters::have_basic_id_info(void) const
{
    return strlen(g.uas_id) > 0 && g.id_type > 0 && g.ua_type > 0;
}

/*
  检查 BasicID 2（第二标识）是否已完整配置

  ASTM F3411 标准允许广播两个 BasicID，
  此函数验证第二组标识信息是否有效。
 */
bool Parameters::have_basic_id_2_info(void) const
{
    return strlen(g.uas_id_2) > 0 && g.id_type_2 > 0 && g.ua_type_2 > 0;
}

/*
  按名称设置 UINT8 类型参数

  查找参数并设置值，返回 false 表示参数名不存在。
 */
bool Parameters::set_by_name_uint8(const char *name, uint8_t v)
{
    const auto *f = find(name);
    if (!f) {
        return false;
    }
    f->set_uint8(v);
    return true;
}

/*
  按名称设置 INT8 类型参数

  查找参数并设置值，返回 false 表示参数名不存在。
 */
bool Parameters::set_by_name_int8(const char *name, int8_t v)
{
    const auto *f = find(name);
    if (!f) {
        return false;
    }
    f->set_int8(v);
    return true;
}

/*
  按名称设置 CHAR64 类型参数

  主要用于设置公钥字符串，查找参数并设置值。
  返回 false 表示参数名不存在。
 */
bool Parameters::set_by_name_char64(const char *name, const char *s)
{
    const auto *f = find(name);
    if (!f) {
        return false;
    }
    f->set_char64(s);
    return true;
}

/*
  按名称设置参数（通用字符串输入版本）

  接受字符串形式的值，根据参数实际类型自动解析转换：
  - 数值类型：通过 strtoul/atof 解析字符串
  - 字符串类型：直接设置
  用于 Web 界面和 DroneCAN GetSet 的字符串值处理。
 */
bool Parameters::set_by_name_string(const char *name, const char *s)
{
    const auto *f = find(name);
    if (!f) {
        return false;
    }
    switch (f->ptype) {
        case ParamType::UINT8:
            f->set_uint8(uint8_t(strtoul(s, nullptr, 0)));
            return true;
        case ParamType::INT8:
            f->set_int8(int8_t(strtoul(s, nullptr, 0)));
            return true;
        case ParamType::UINT32:
            f->set_uint32(strtoul(s, nullptr, 0));
            return true;
        case ParamType::FLOAT:
            f->set_float(atof(s));
            return true;
        case ParamType::CHAR20:
            f->set_char20(s);
            return true;
        case ParamType::CHAR64:
            f->set_char64(s);
            return true;
    }
    return false;
}

/*
  获取第 i 个公钥（Base64 解码为32字节原始密钥）

  公钥在 NVS 中以 "PUBLIC_KEYV1:<base64_data>" 格式存储。
  此函数验证前缀格式，然后将 Base64 部分解码为32字节的
  Ed25519 公钥原始数据。

  参数：
    i    - 公钥槽索引（0 到 MAX_PUBLIC_KEYS-1）
    key  - 输出缓冲区，32字节

  返回 false 表示：索引越界、前缀格式不匹配、或解码长度不是32字节。
 */
bool Parameters::get_public_key(uint8_t i, uint8_t key[32]) const
{
    if (i >= MAX_PUBLIC_KEYS) {
        return false;
    }
    const char *b64_key = g.public_keys[i].b64_key;

    // 验证公钥格式前缀 "PUBLIC_KEYV1:"
    const char *ktype = "PUBLIC_KEYV1:";
    if (strncmp(b64_key, ktype, strlen(ktype)) != 0) {
        return false; // 前缀不匹配，该槽为空或格式无效
    }

    // 跳过前缀，对 Base64 数据部分进行解码
    b64_key += strlen(ktype);
    int32_t out_len = base64_decode(b64_key, key, 32);
    if (out_len != 32) {
        return false; // 解码结果不是32字节，公钥数据损坏
    }
    return true;
}

/*
  检查是否没有任何有效公钥

  遍历所有公钥槽，如果全部为空或无效则返回 true。
  当没有公钥时，安全签名验证将被跳过（允许未签名命令）。
 */
bool Parameters::no_public_keys(void) const
{
    for (uint8_t i=0; i<MAX_PUBLIC_KEYS; i++) {
        uint8_t key[32];
        if (get_public_key(i, key)) {
            return false; // 找到至少一个有效公钥
        }
    }
    return true; // 所有槽均为空或无效
}

/*
  设置第 i 个公钥（将32字节原始密钥 Base64 编码后存储）

  将原始的 Ed25519 公钥编码为 Base64 字符串，
  然后以 "PUBLIC_KEYV1:<base64>" 格式写入对应的参数槽。

  参数：
    i    - 公钥槽索引（0 到 MAX_PUBLIC_KEYS-1）
    key  - 32字节的 Ed25519 公钥原始数据

  返回 false 表示：索引越界或 Base64 编码失败。
 */
bool Parameters::set_public_key(uint8_t i, const uint8_t key[32])
{
    if (i >= MAX_PUBLIC_KEYS) {
        return false;
    }
    // 将32字节公钥编码为 Base64 字符串
    char *s = base64_encode(key, PUBLIC_KEY_LEN);
    if (s == nullptr) {
        return false;
    }
    // 构造参数名 "PUBLIC_KEYx"（x 为 1-5）
    char name[] = "PUBLIC_KEYx";
    name[strlen(name)-2] = '1'+i;
    bool ret = set_by_name_char64(name, s);
    delete[] s; // 释放 base64_encode 分配的内存
    return ret;
}

/*
  删除第 i 个公钥（将对应槽清空）

  通过将参数值设为空字符串来移除公钥。
  用于安全命令中的公钥管理操作。
 */
bool Parameters::remove_public_key(uint8_t i)
{
    if (i >= MAX_PUBLIC_KEYS) {
        return false;
    }
    // 构造参数名 "PUBLIC_KEYx"（x 为 1-5）
    char name[] = "PUBLIC_KEYx";
    name[strlen(name)-2] = '1'+i;
    return set_by_name_char64(name, ""); // 设为空字符串即为删除
}
