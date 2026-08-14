/*
  handle status web page
 */

/*
 * ============================================================================
 * status.cpp — Web状态页面JSON数据生成器
 * ============================================================================
 *
 * 本文件为Web管理界面的AJAX状态查询端点生成JSON格式的状态数据。
 * 前端页面通过定时轮询此端点获取设备的实时远程识别(Remote ID)信息。
 *
 * 主要功能:
 *   1. 定义ODID (OpenDroneID)协议中所有枚举值到可读字符串的映射表
 *   2. 提供辅助函数: JSON格式化、枚举查找、经纬度/高度的字符串转换
 *   3. status_json() 函数构建完整的JSON对象，包含以下数据段:
 *      - STATUS:   固件版本、硬件ID、运行时长、剩余内存
 *      - BASICID:  无人机类型(UA Type)、ID类型、UAS标识 (支持双ID)
 *      - OPERATORID: 操作员ID信息
 *      - SELFID:   自身描述信息
 *      - SYSTEM:   系统级数据(操作员位置、分类、区域信息、EU分类)
 *      - LOCATION: 位置数据(状态、速度、经纬度、高度、各项精度)
 *
 * 依赖: opendroneid.h 提供的ODID枚举定义和 ODID_UAS_Data 结构体
 * ============================================================================
 */

#include "options.h"
#include <Arduino.h>
#include "version.h"
#include <opendroneid.h>
#include "status.h"
#include "util.h"

/* 外部全局变量: UAS_data 包含所有远程识别数据，由MAVLink或DroneCAN模块更新 */
extern ODID_UAS_Data UAS_data;

/* 外部全局变量: 当前状态的原因描述字符串(如故障原因等) */
extern String status_reason;

/*
 * JSON键值对结构体 —— 用于构建JSON对象的基本单元
 * name:  JSON字段名 (格式为 "段名:字段名", 例如 "LOCATION:Latitude")
 * value: JSON字段值 (字符串形式)
 */
typedef struct {
    String name;
    String value;
} json_table_t;

/*
 * escape_string() — JSON字符串转义函数
 * 移除字符串中的双引号字符，防止破坏JSON格式的完整性。
 * 注意: 此处采用直接删除而非转义(\")的简化处理方式。
 */
static String escape_string(String s)
{
    s.replace("\"", "");
    return s;
}

/*
 * json_format() — 将键值对数组序列化为JSON字符串
 *
 * 遍历 json_table_t 数组，生成形如 {"key1":"val1","key2":"val2",...} 的JSON对象。
 * 所有值都以字符串类型输出(用双引号包裹)，值中的双引号会被 escape_string 移除。
 *
 * @param table  json_table_t 数组指针
 * @param n      数组元素个数
 * @return       格式化后的JSON字符串
 */
static String json_format(const json_table_t *table, uint8_t n)
{
    String s = "{";
    for (uint8_t i=0; i<n; i++) {
        const auto &t = table[i];
        s += "\"" + t.name + "\" : ";
        s += "\"" + escape_string(t.value) + "\"";
        if (i != n-1) {
            s += ",";
        }
    }
    s += "}";
    return s;
}

/*
 * enum_map_t — 枚举值映射结构体
 * 用于将ODID协议中的整数枚举值映射为人类可读的字符串。
 * v: 枚举整数值
 * s: 对应的字符串描述
 */
typedef struct {
    int v;
    String s;
} enum_map_t;

/*
 * 无人机类型(UA Type)枚举映射表
 * 定义 ODID_UATYPE_xxx 枚举值到字符串的映射。
 * 涵盖: 固定翼(AEROPLANE)、直升机/多旋翼、旋翼机、混合升力、
 *        扑翼机、滑翔机、风筝、气球、飞艇、降落伞、火箭、
 *        系留飞行器、地面障碍物等类型。
 */
static const enum_map_t enum_uatype[] = {
    { ODID_UATYPE_NONE , "NONE" },
    { ODID_UATYPE_AEROPLANE , "AEROPLANE" },
    { ODID_UATYPE_HELICOPTER_OR_MULTIROTOR , "HELICOPTER_OR_MULTIROTOR" },
    { ODID_UATYPE_GYROPLANE , "GYROPLANE" },
    { ODID_UATYPE_HYBRID_LIFT , "HYBRID_LIFT" },
    { ODID_UATYPE_ORNITHOPTER , "ORNITHOPTER" },
    { ODID_UATYPE_GLIDER , "GLIDER" },
    { ODID_UATYPE_KITE , "KITE" },
    { ODID_UATYPE_FREE_BALLOON , "FREE_BALLOON" },
    { ODID_UATYPE_CAPTIVE_BALLOON , "CAPTIVE_BALLOON" },
    { ODID_UATYPE_AIRSHIP , "AIRSHIP" },
    { ODID_UATYPE_FREE_FALL_PARACHUTE , "FREE_FALL_PARACHUTE" },
    { ODID_UATYPE_ROCKET , "ROCKET" },
    { ODID_UATYPE_TETHERED_POWERED_AIRCRAFT , "TETHERED_POWERED_AIRCRAFT" },
    { ODID_UATYPE_GROUND_OBSTACLE , "GROUND_OBSTACLE" },
    { ODID_UATYPE_OTHER , "OTHER" },
};

/*
 * ID类型(ID Type)枚举映射表
 * 定义无人机标识符的类型:
 *   NONE — 未设置
 *   SERIAL_NUMBER — 制造商序列号 (ANSI/CTA-2063-A格式)
 *   CAA_REGISTRATION_ID — 民航局注册号
 *   UTM_ASSIGNED_UUID — UTM系统分配的UUID
 *   SPECIFIC_SESSION_ID — 特定会话ID
 */
static const enum_map_t enum_idtype[] = {
    { ODID_IDTYPE_NONE , "NONE" },
    { ODID_IDTYPE_SERIAL_NUMBER , "SERIAL_NUMBER" },
    { ODID_IDTYPE_CAA_REGISTRATION_ID , "CAA_REGISTRATION_ID" },
    { ODID_IDTYPE_UTM_ASSIGNED_UUID , "UTM_ASSIGNED_UUID" },
    { ODID_IDTYPE_SPECIFIC_SESSION_ID , "SPECIFIC_SESSION_ID" },
};

/*
 * 操作员位置类型(Operator Location Type)枚举映射表
 * 定义操作员位置信息的来源:
 *   TAKEOFF — 起飞位置
 *   LIVE_GNSS — 实时GNSS定位 (如操作员手持设备GPS)
 *   FIXED — 固定位置 (预设坐标)
 */
static const enum_map_t enum_loctype[] = {
    { ODID_OPERATOR_LOCATION_TYPE_TAKEOFF , "TAKEOFF" },
    { ODID_OPERATOR_LOCATION_TYPE_LIVE_GNSS , "LIVE_GNSS" },
    { ODID_OPERATOR_LOCATION_TYPE_FIXED , "FIXED" },
};

/*
 * 分类体系类型(Classification Type)枚举映射表
 * 定义无人机所适用的监管分类体系:
 *   UNDECLARED — 未声明
 *   EU — 欧盟分类体系 (配合 CategoryEU 和 ClassEU 使用)
 */
static const enum_map_t enum_classif[] = {
    { ODID_CLASSIFICATION_TYPE_UNDECLARED , "UNDECLARED" },
    { ODID_CLASSIFICATION_TYPE_EU , "EU" },
};

/*
 * 无人机运行状态(Status)枚举映射表
 * 定义无人机当前的运行状态:
 *   UNDECLARED — 未声明
 *   GROUND — 地面状态 (未起飞)
 *   AIRBORNE — 空中飞行
 *   EMERGENCY — 紧急状态
 *   REMOTE_ID_SYSTEM_FAILURE — 远程识别系统故障
 */
static const enum_map_t enum_status[] = {
    { ODID_STATUS_UNDECLARED , "UNDECLARED" },
    { ODID_STATUS_GROUND , "GROUND" },
    { ODID_STATUS_AIRBORNE , "AIRBORNE" },
    { ODID_STATUS_EMERGENCY , "EMERGENCY" },
    { ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE , "REMOTE_ID_SYSTEM_FAILURE" },
};

/*
 * 高度参考基准(Height Reference)枚举映射表
 * 定义高度值的参考基准:
 *   OVER_TAKEOFF — 相对于起飞点的高度 (AGL起飞点)
 *   OVER_GROUND — 相对于地面的高度 (AGL正下方地面)
 */
static const enum_map_t enum_height[] = {
    { ODID_HEIGHT_REF_OVER_TAKEOFF , "OVER_TAKEOFF" },
    { ODID_HEIGHT_REF_OVER_GROUND , "OVER_GROUND" },
};

/*
 * 水平精度(Horizontal Accuracy)枚举映射表
 * 定义水平位置精度等级，从最低精度到最高精度:
 *   UNKNOWN — 未知精度
 *   10NM ~ 0.05NM — 以海里为单位的精度级别
 *   30m ~ 1m — 以米为单位的精度级别
 */
static const enum_map_t enum_hacc[] = {
    { ODID_HOR_ACC_UNKNOWN , "UNKNOWN" },
    { ODID_HOR_ACC_10NM , "10 nm" },
    { ODID_HOR_ACC_4NM , "4 nm" },
    { ODID_HOR_ACC_2NM , "2 nm" },
    { ODID_HOR_ACC_1NM , "1 nm" },
    { ODID_HOR_ACC_0_5NM , "0.5 nm" },
    { ODID_HOR_ACC_0_3NM , "0.3 nm" },
    { ODID_HOR_ACC_0_1NM , "0.1 nm" },
    { ODID_HOR_ACC_0_05NM , "0.05 nm" },
    { ODID_HOR_ACC_30_METER , "30 m" },
    { ODID_HOR_ACC_10_METER , "10 m" },
    { ODID_HOR_ACC_3_METER , "3 m" },
    { ODID_HOR_ACC_1_METER , "1 m" },
};

/*
 * 垂直精度(Vertical Accuracy)枚举映射表
 * 定义垂直位置(高度)精度等级:
 *   UNKNOWN — 未知精度
 *   150m ~ 1m — 精度从低到高
 * 注意: 此映射表同时用于气压高度精度(BaroAccuracy)和几何高度精度(VertAccuracy)
 */
static const enum_map_t enum_vacc[] = {
    { ODID_VER_ACC_UNKNOWN , "UNKNOWN" },
    { ODID_VER_ACC_150_METER , "150 m" },
    { ODID_VER_ACC_45_METER , "45 m" },
    { ODID_VER_ACC_25_METER , "25 m" },
    { ODID_VER_ACC_10_METER , "10 m" },
    { ODID_VER_ACC_3_METER , "3 m" },
    { ODID_VER_ACC_1_METER , "1 m" },
};

/*
 * 速度精度(Speed Accuracy)枚举映射表
 * 定义速度测量精度等级:
 *   UNKNOWN — 未知精度
 *   10m/s ~ 0.3m/s — 速度精度从低到高
 */
static const enum_map_t enum_sacc[] = {
    { ODID_SPEED_ACC_UNKNOWN , "UNKNOWN" },
    { ODID_SPEED_ACC_10_METERS_PER_SECOND , "10 m/s" },
    { ODID_SPEED_ACC_3_METERS_PER_SECOND , "3 m/s" },
    { ODID_SPEED_ACC_1_METERS_PER_SECOND , "1 m/s" },
    { ODID_SPEED_ACC_0_3_METERS_PER_SECOND , "0.3 m/s" },
};

/*
 * 描述类型(Description Type)枚举映射表
 * 定义SelfID消息中描述信息的类型:
 *   TEXT — 普通文本描述
 *   EMERGENCY — 紧急状态描述
 *   EXTENDED_STATUS — 扩展状态描述
 */
static const enum_map_t enum_desctype[] = {
    { ODID_DESC_TYPE_TEXT , "TEXT" },
    { ODID_DESC_TYPE_EMERGENCY , "EMERGENCY" },
    { ODID_DESC_TYPE_EXTENDED_STATUS , "EXTENDED_STATUS" },
};

/*
 * 欧盟无人机等级(EU Class)枚举映射表
 * 定义欧盟法规下的无人机等级 (C0 ~ C6):
 *   UNDECLARED — 未声明
 *   CLASS_0 ~ CLASS_6 — 对应欧盟无人机C0至C6等级标识
 * 等级决定了无人机的运行限制和远程识别要求
 */
static const enum_map_t enum_classeu[] = {
    { ODID_CLASS_EU_UNDECLARED , "UNDECLARED" },
    { ODID_CLASS_EU_CLASS_0 , "CLASS_0" },
    { ODID_CLASS_EU_CLASS_1 , "CLASS_1" },
    { ODID_CLASS_EU_CLASS_2 , "CLASS_2" },
    { ODID_CLASS_EU_CLASS_3 , "CLASS_3" },
    { ODID_CLASS_EU_CLASS_4 , "CLASS_4" },
    { ODID_CLASS_EU_CLASS_5 , "CLASS_5" },
    { ODID_CLASS_EU_CLASS_6 , "CLASS_6" },
};

/*
 * 欧盟运行类别(EU Category)枚举映射表
 * 定义欧盟法规下的运行类别:
 *   UNDECLARED — 未声明
 *   OPEN — 开放类 (低风险，无需授权)
 *   SPECIFIC — 特定类 (中等风险，需运行授权)
 *   CERTIFIED — 认证类 (高风险，需型号认证)
 */
static const enum_map_t enum_cateu[] = {
    { ODID_CATEGORY_EU_UNDECLARED , "UNDECLARED" },
    { ODID_CATEGORY_EU_OPEN , "OPEN" },
    { ODID_CATEGORY_EU_SPECIFIC , "SPECIFIC" },
    { ODID_CATEGORY_EU_CERTIFIED , "CERTIFIED" },
};

/*
 * 时间戳精度(Timestamp Accuracy)枚举映射表
 * 定义位置时间戳的精度等级:
 *   UNKNOWN — 未知精度
 *   0.1s ~ 1.5s — 时间精度从高到低 (步长0.1秒)
 */
static const enum_map_t enum_tsacc[] = {
    { ODID_TIME_ACC_UNKNOWN , "UNKNOWN" },
    { ODID_TIME_ACC_0_1_SECOND , "0.1 s" },
    { ODID_TIME_ACC_0_2_SECOND , "0.2 s" },
    { ODID_TIME_ACC_0_3_SECOND , "0.3 s" },
    { ODID_TIME_ACC_0_4_SECOND , "0.4 s" },
    { ODID_TIME_ACC_0_5_SECOND , "0.5 s" },
    { ODID_TIME_ACC_0_6_SECOND , "0.6 s" },
    { ODID_TIME_ACC_0_7_SECOND , "0.7 s" },
    { ODID_TIME_ACC_0_8_SECOND , "0.8 s" },
    { ODID_TIME_ACC_0_9_SECOND , "0.9 s" },
    { ODID_TIME_ACC_1_0_SECOND , "1.0 s" },
    { ODID_TIME_ACC_1_1_SECOND , "1.1 s" },
    { ODID_TIME_ACC_1_2_SECOND , "1.2 s" },
    { ODID_TIME_ACC_1_3_SECOND , "1.3 s" },
    { ODID_TIME_ACC_1_4_SECOND , "1.4 s" },
    { ODID_TIME_ACC_1_5_SECOND , "1.5 s" },
};

/*
 * enum_string() — 枚举值到字符串的通用查找函数
 *
 * 在给定的枚举映射表中线性搜索匹配的枚举值，返回对应的字符串。
 * 如果未找到匹配项，则将枚举整数值直接转为字符串返回，作为兜底处理。
 *
 * @param m  枚举映射表数组指针
 * @param n  映射表元素个数
 * @param v  待查找的枚举整数值
 * @return   匹配的字符串描述，或枚举值的数字字符串
 */
static String enum_string(const enum_map_t *m, uint8_t n, int v)
{
    for (uint8_t i=0; i<n; i++) {
        if (m[i].v == v) {
            return m[i].s;
        }
    }
    return String(v);
}

/**
 * LatLonString() — 经纬度格式化函数
 *
 * 将经纬度坐标转换为字符串表示。当经度和纬度都为0时，
 * 视为无效/未设置数据，返回 "UNKNOWN"。
 * 有效坐标保留8位小数精度 (约1毫米级)。
 *
 * @param lat    纬度值 (度)
 * @param lon    经度值 (度)
 * @param select 选择返回哪个坐标: 0=纬度, 1=经度
 * @return       坐标字符串或 "UNKNOWN"
 */
static String LatLonString(double lat, double lon, uint8_t select)
{
    if (lat != 0.0 || lon != 0.0) {
        switch (select) {
        case 0:  // lat
            return String(lat, 8);
        case 1:  // lon
            return String(lon, 8);
        default:
            break;
        }
    }
    return "UNKNOWN";
}

/**
 * AltString() — 高度值格式化函数
 *
 * 将高度值转换为字符串表示。ODID协议中高度值 <= -1000.0
 * 表示数据无效/未设置，此时返回 "UNKNOWN"。
 * 有效高度保留2位小数精度。
 *
 * @param alt  高度值 (米)
 * @return     高度字符串或 "UNKNOWN"
 */
static String AltString(float alt)
{
    if (alt <= -1000.0f) {
        return "UNKNOWN";
    }
    return String(alt,2);
}

/*
 * ENUM_MAP 宏 — 简化枚举映射查找的便捷宏
 * 利用 ## 运算符拼接枚举表名称，自动计算数组大小，
 * 调用 enum_string() 进行查找。
 * 用法示例: ENUM_MAP(uatype, value) 展开为
 *   enum_string(enum_uatype, ARRAY_SIZE(enum_uatype), int(value))
 */
#define ENUM_MAP(ename, v) enum_string(enum_ ## ename, ARRAY_SIZE(enum_ ## ename), int(v))

/*
 * status_json() — 生成完整的设备状态JSON字符串
 *
 * 此函数是Web状态页面的核心数据接口。将当前所有远程识别(RID)数据
 * 组装为一个JSON对象返回给前端AJAX请求。
 *
 * JSON对象包含以下数据段:
 *
 * [STATUS段] — 设备基本状态
 *   VERSION:   固件版本号 + Git提交哈希
 *   BOARD_ID:  硬件板卡标识
 *   UPTIME:    系统运行时长 (时:分:秒格式)
 *   FREEMEM:   ESP32剩余堆内存 (字节)
 *
 * [BASICID段] — 无人机基本标识 (支持双ID)
 *   UAType/UAType2:  无人机类型
 *   IDType/IDType2:  标识类型
 *   UASID/UASID2:    UAS标识字符串
 *
 * [OPERATORID段] — 操作员标识
 *   IDType:  操作员ID类型
 *   ID:      操作员ID字符串
 *
 * [SELFID段] — 自身描述信息
 *   DescType:  描述类型
 *   Desc:      描述文本
 *
 * [SYSTEM段] — 系统级信息
 *   OperatorLocationType:  操作员位置类型
 *   ClassificationType:    分类体系类型
 *   OperatorLatitude/Longitude: 操作员经纬度
 *   AreaCount/Radius/Ceiling/Floor: 运行区域参数
 *   CategoryEU/ClassEU:    欧盟分类信息
 *   OperatorAltitudeGeo:   操作员海拔高度
 *   Timestamp:             系统时间戳
 *
 * [LOCATION段] — 无人机位置与运动信息
 *   Status/StatusReason:   运行状态及原因
 *   Direction:             航向 (度)
 *   SpeedHorizontal/Vertical: 水平/垂直速度
 *   Latitude/Longitude:    无人机经纬度
 *   AltitudeBaro/AltitudeGeo: 气压/几何高度
 *   HeightType/Height:     高度类型及高度值
 *   HorizAccuracy/VertAccuracy/BaroAccuracy: 位置精度
 *   SpeedAccuracy/TSAccuracy: 速度和时间戳精度
 *   TimeStamp:             位置时间戳
 *
 * @return  完整的JSON字符串
 */
String status_json(void)
{
    /* 计算系统运行时长，拆分为时、分、秒 */
    const uint32_t now_s = millis() / 1000;
    const uint32_t sec = now_s % 60;
    const uint32_t min = (now_s / 60) % 60;
    const uint32_t hr = (now_s / 3600) % 24;
    char minsec_str[6] {};  // HOUR does not include. Because wired powered drones allow for longer flight times.
    snprintf(minsec_str, sizeof(minsec_str), "%02d:%02d", min, sec);

    /* 格式化Git版本哈希为括号包裹的十六进制字符串 */
    char githash[20];
    snprintf(githash, sizeof(githash), "(%08x)", GIT_VERSION);

    /* 构建状态原因字符串，非空时用括号包裹 */
    String reason = "";
    if (status_reason != nullptr && status_reason.length() > 0) {
        reason = "(" + status_reason + ")";
    }

    /*
     * JSON数据表 —— 按段组织所有远程识别数据字段
     * 键名格式: "段名:字段名"，前端根据冒号分割解析段归属
     */
    const json_table_t table[] = {
        /* ---- STATUS段: 设备基本信息 ---- */
        { "STATUS:VERSION", String(FW_VERSION_MAJOR) + "." + String(FW_VERSION_MINOR) + " " + githash},
        { "STATUS:BOARD_ID", String(BOARD_ID)},
        { "STATUS:UPTIME", String(hr) + ":" + String(minsec_str) },
        { "STATUS:FREEMEM", String(ESP.getFreeHeap()) },

        /* ---- BASICID段: 主无人机标识(ID[0]) ---- */
        { "BASICID:UAType", ENUM_MAP(uatype, UAS_data.BasicID[0].UAType) },
        { "BASICID:IDType", ENUM_MAP(idtype, UAS_data.BasicID[0].IDType) },
        { "BASICID:UASID", String(UAS_data.BasicID[0].UASID) },

        /* ---- BASICID段: 备用无人机标识(ID[1]) ---- */
        { "BASICID:UAType2", ENUM_MAP(uatype, UAS_data.BasicID[1].UAType) },
        { "BASICID:IDType2", ENUM_MAP(idtype, UAS_data.BasicID[1].IDType) },
        { "BASICID:UASID2", String(UAS_data.BasicID[1].UASID) },

        /* ---- OPERATORID段: 操作员标识信息 ---- */
        { "OPERATORID:IDType", String(UAS_data.OperatorID.OperatorIdType) },
        { "OPERATORID:ID", String(UAS_data.OperatorID.OperatorId) },

        /* ---- SELFID段: 自身描述信息 ---- */
        { "SELFID:DescType", ENUM_MAP(desctype, UAS_data.SelfID.DescType) },
        { "SELFID:Desc", String(UAS_data.SelfID.Desc) },

        /* ---- SYSTEM段: 系统级数据(操作员位置、分类、区域) ---- */
        { "SYSTEM:OperatorLocationType", ENUM_MAP(loctype, UAS_data.System.OperatorLocationType) },
        { "SYSTEM:ClassificationType", ENUM_MAP(classif, UAS_data.System.ClassificationType) },
        { "SYSTEM:OperatorLatitude", LatLonString(UAS_data.System.OperatorLatitude, UAS_data.System.OperatorLongitude, 0) },
        { "SYSTEM:OperatorLongitude", LatLonString(UAS_data.System.OperatorLatitude, UAS_data.System.OperatorLongitude, 1) },
        { "SYSTEM:AreaCount", String(UAS_data.System.AreaCount) },
        { "SYSTEM:AreaRadius", String(UAS_data.System.AreaRadius) },
        { "SYSTEM:AreaCeiling", AltString(UAS_data.System.AreaCeiling) },
        { "SYSTEM:AreaFloor", AltString(UAS_data.System.AreaFloor) },
        { "SYSTEM:CategoryEU", ENUM_MAP(cateu, UAS_data.System.CategoryEU) },
        { "SYSTEM:ClassEU", ENUM_MAP(classeu, UAS_data.System.ClassEU) },
        { "SYSTEM:OperatorAltitudeGeo", AltString(UAS_data.System.OperatorAltitudeGeo) },
        { "SYSTEM:Timestamp", String(UAS_data.System.Timestamp) },

        /* ---- LOCATION段: 无人机位置与运动数据 ---- */
        { "LOCATION:Status", ENUM_MAP(status, UAS_data.Location.Status)},
        { "LOCATION:StatusReason", reason },
        { "LOCATION:Direction", String(UAS_data.Location.Direction) },
        { "LOCATION:SpeedHorizontal", String(UAS_data.Location.SpeedHorizontal) },
        { "LOCATION:SpeedVertical", String(UAS_data.Location.SpeedVertical) },
        { "LOCATION:Latitude", LatLonString(UAS_data.Location.Latitude, UAS_data.Location.Longitude, 0) },
        { "LOCATION:Longitude", LatLonString(UAS_data.Location.Latitude, UAS_data.Location.Longitude, 1) },
        { "LOCATION:AltitudeBaro", AltString(UAS_data.Location.AltitudeBaro) },
        { "LOCATION:AltitudeGeo", AltString(UAS_data.Location.AltitudeGeo) },
        { "LOCATION:HeightType", ENUM_MAP(height, UAS_data.Location.HeightType) },
        { "LOCATION:Height", AltString(UAS_data.Location.Height) },
        { "LOCATION:HorizAccuracy", ENUM_MAP(hacc, UAS_data.Location.HorizAccuracy) },
        { "LOCATION:VertAccuracy", ENUM_MAP(vacc, UAS_data.Location.VertAccuracy) },
        { "LOCATION:BaroAccuracy", ENUM_MAP(vacc, UAS_data.Location.BaroAccuracy) },
        { "LOCATION:SpeedAccuracy", ENUM_MAP(sacc, UAS_data.Location.SpeedAccuracy) },
        { "LOCATION:TSAccuracy", ENUM_MAP(tsacc, UAS_data.Location.TSAccuracy) },
        { "LOCATION:TimeStamp", String(UAS_data.Location.TimeStamp) },
    };
    return json_format(table, ARRAY_SIZE(table));
}
