/*
  参数管理系统

  本模块定义所有可配置参数及其存储、读写接口。
  参数通过 DroneCAN GetSet 或 MAVLink PARAM_SET 协议远程读写，
  存储在 ESP32 的 NVS (Non-Volatile Storage) 中。

  参数类型：
  - UINT8/INT8：8 位整数（如 can_node, lock_level）
  - UINT32：32 位整数（如 baudrate）
  - FLOAT：浮点数（如 wifi_power, bt4_rate）
  - CHAR20：20 字符字符串（如 uas_id, wifi_ssid）
  - CHAR64：64 字符字符串（如 public_keys）

  安全机制：
  - lock_level：参数锁定等级（0=未锁定，1=参数锁定，2=烧录 eFuse 禁用调试接口）
  - public_keys：Ed25519 公钥列表，用于安全命令的签名验证
  - PARAM_FLAG_PASSWORD：密码类型参数在读取时返回掩码
  - PARAM_FLAG_HIDDEN：隐藏参数不显示在参数列表中
 */
#pragma once

#include <stdint.h>
#include "board_config.h"

#define MAX_PUBLIC_KEYS 5           // 最大公钥数量（Ed25519 签名验证）
#define PUBLIC_KEY_LEN 32           // 每个公钥长度（字节）
#define PARAM_NAME_MAX_LEN 16       // 参数名称最大长度

#define PARAM_FLAG_NONE 0           // 无特殊标志
#define PARAM_FLAG_PASSWORD (1U<<0) // 密码类型：读取时返回 "********" 掩码
#define PARAM_FLAG_HIDDEN (1U<<1)   // 隐藏参数：不在参数列表中显示

// 参数管理类
class Parameters {
public:
    // ==================== 配置参数字段 ====================
#if defined(PIN_CAN_TERM)
    uint8_t can_term = !CAN_TERM_EN;   // CAN 终端电阻使能（默认为板级定义的反值）
#endif
    int8_t lock_level;                  // 参数锁定等级（0=未锁定，1=锁定，2=eFuse 锁定）
    uint8_t can_node;                   // DroneCAN 节点 ID（0=自动分配，1-127=固定）
    uint8_t bcast_powerup;              // 上电后是否立即开始广播
    uint32_t baudrate = 57600;          // MAVLink 串口波特率
    uint8_t ua_type;                    // 无人机类型（ODID_UA_TYPE 枚举）
    uint8_t id_type;                    // ID 类型（ODID_ID_TYPE 枚举）
    char uas_id[21] = "ABCD123456789"; // UAS 标识符（如序列号/注册号）
    uint8_t ua_type_2;                  // 第二 BasicID 的无人机类型
    uint8_t id_type_2;                  // 第二 BasicID 的 ID 类型
    char uas_id_2[21] = "ABCD123456789"; // 第二 BasicID 的 UAS 标识符
    float wifi_nan_rate;                // WiFi NAN 广播速率（Hz）
    float wifi_beacon_rate;             // WiFi Beacon 广播速率（Hz）
    float wifi_power;                   // WiFi 发射功率（dBm）
    float bt4_rate;                     // BT4 Legacy 广播速率（Hz）
    float bt4_power;                    // BT4 发射功率（dBm）
    float bt5_rate;                     // BT5 Long Range 广播速率（Hz）
    float bt5_power;                    // BT5 发射功率（dBm）
    uint8_t done_init;                  // 是否已完成首次初始化
    uint8_t webserver_enable;           // Web 管理界面使能
    uint8_t mavlink_sysid;              // MAVLink 系统 ID（0=自动从心跳获取）
    char wifi_ssid[21] = "ArduRemoteID";     // WiFi 热点名称（Web 界面用）
    char wifi_password[21] = "ArduRemoteID"; // WiFi 热点密码
    uint8_t wifi_channel = 6;           // WiFi 信道
    uint8_t to_factory_defaults = 0;    // 恢复出厂设置标志
    uint8_t options;                    // 选项位掩码（见 OPTIONS_* 定义）
    struct {
        char b64_key[64];               // Base64 编码的 Ed25519 公钥
    } public_keys[MAX_PUBLIC_KEYS];     // 公钥列表

    // ==================== 参数类型枚举 ====================
    enum class ParamType {
        NONE=0,     // 参数列表结束标记
        UINT8=1,    // 8 位无符号整数
        UINT32=2,   // 32 位无符号整数
        FLOAT=3,    // 32 位浮点数
        CHAR20=4,   // 20 字符字符串
        CHAR64=5,   // 64 字符字符串
        INT8=6,     // 8 位有符号整数
    };

    // ==================== 参数描述符结构 ====================
    struct Param {
        char name[PARAM_NAME_MAX_LEN+1];  // 参数名称
        ParamType ptype;                   // 参数类型
        const void *ptr;                   // 指向参数值的指针
        float default_value;               // 默认值
        float min_value;                   // 最小值
        float max_value;                   // 最大值
        uint16_t flags;                    // 标志位（PARAM_FLAG_*）
        uint8_t min_len;                   // 字符串参数的最小长度

        // 参数值写入方法
        void set_float(float v) const;
        void set_uint8(uint8_t v) const;
        void set_int8(int8_t v) const;
        void set_uint32(uint32_t v) const;
        void set_char20(const char *v) const;
        void set_char64(const char *v) const;

        // 参数值读取方法
        uint8_t get_uint8() const;
        int8_t get_int8() const;
        uint32_t get_uint32() const;
        float get_float() const;
        const char *get_char20() const;
        const char *get_char64() const;

        // 通用浮点数接口（MAVLink 参数协议使用）
        bool get_as_float(float &v) const;
        void set_as_float(float v) const;
    };
    static const struct Param params[];     // 参数描述符表（定义在 parameters.cpp 中）

    // ==================== 参数查找方法 ====================
    static const Param *find(const char *name);             // 按名称查找参数
    static const Param *find_by_index(uint16_t idx);        // 按索引查找（所有类型）
    static const Param *find_by_index_float(uint16_t idx);  // 按索引查找（仅浮点兼容类型）

    void init(void);    // 从 NVS 加载参数值

    bool have_basic_id_info(void) const;    // 检查 BasicID 1 是否已配置
    bool have_basic_id_2_info(void) const;  // 检查 BasicID 2 是否已配置

    // ==================== 按名称设置参数 ====================
    bool set_by_name_uint8(const char *name, uint8_t v);
    bool set_by_name_int8(const char *name, int8_t v);
    bool set_by_name_char64(const char *name, const char *s);
    bool set_by_name_string(const char *name, const char *s);  // 通用字符串设置（自动匹配类型）

    // ==================== 公钥管理 ====================
    bool get_public_key(uint8_t i, uint8_t key[32]) const;  // 获取第 i 个公钥（Base64 解码）
    bool set_public_key(uint8_t i, const uint8_t key[32]);  // 设置第 i 个公钥（Base64 编码存储）
    bool remove_public_key(uint8_t i);                       // 删除第 i 个公钥
    bool no_public_keys(void) const;                         // 检查是否无任何公钥

    // ==================== MAVLink 参数协议辅助 ====================
    static uint16_t param_count_float(void);                 // 浮点兼容参数总数
    static int16_t param_index_float(const Param *p);        // 参数在浮点列表中的索引

private:
    void load_defaults(void);   // 加载出厂默认值
};

// ==================== OPTIONS 选项位掩码定义 ====================
#define OPTIONS_FORCE_ARM_OK (1U<<0)                        // 强制解锁就绪（跳过数据验证）
#define OPTIONS_DONT_SAVE_BASIC_ID_TO_PARAMETERS (1U<<1)    // 不将 BasicID 保存到 NVS
#define OPTIONS_PRINT_RID_MAVLINK (1U<<2)                   // 打印接收到的 MAVLink RID 消息

extern Parameters g;    // 全局参数实例
