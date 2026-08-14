/*
  DroneCAN 通信协议驱动实现

  本模块通过 DroneCAN（基于 CAN 总线的无人机通信协议）与飞控通信，
  接收 OpenDroneID 远程识别数据并管理节点状态。

  主要功能：
  1. CAN 总线初始化（含接收过滤器，过滤高优先级的 ESC 命令等高频消息）
  2. 动态节点 ID 分配（DNA）— 遵循 UAVCAN 协议自动获取节点 ID
  3. 接收 RemoteID 消息（BasicID/Location/SelfID/System/OperatorID）
  4. 参数读写（GetSet）— 支持 UINT8/INT8/UINT32/FLOAT/CHAR20/CHAR64 类型
  5. 安全命令处理（SecureCommand）— 支持会话密钥获取和配置写入
  6. 发送节点状态和解锁状态广播（1Hz）
  7. 通过 CAN LogMessage 发送调试信息

  致谢：David Buzz 提供的 ArduPilot ESP32 HAL 参考
 */
#include <Arduino.h>
#include "board_config.h"
#include "version.h"
#include <time.h>
#include "DroneCAN.h"
#include "parameters.h"
#include <stdarg.h>
#include "util.h"
#include "monocypher.h"

// DroneCAN/UAVCAN 协议消息头文件
#include <canard.h>
#include <uavcan.protocol.NodeStatus.h>
#include <uavcan.protocol.GetNodeInfo.h>
#include <uavcan.protocol.RestartNode.h>
#include <uavcan.protocol.dynamic_node_id.Allocation.h>
#include <uavcan.protocol.param.GetSet.h>
#include <uavcan.protocol.debug.LogMessage.h>
#include <dronecan.remoteid.BasicID.h>
#include <dronecan.remoteid.Location.h>
#include <dronecan.remoteid.SelfID.h>
#include <dronecan.remoteid.System.h>
#include <dronecan.remoteid.OperatorID.h>
#include <dronecan.remoteid.ArmStatus.h>

#ifndef CAN_BOARD_ID
#define CAN_BOARD_ID 10001              // DroneCAN 硬件板 ID（默认值）
#endif

#ifndef CAN_APP_NODE_NAME
#define CAN_APP_NODE_NAME "ArduPilot RemoteIDModule"  // DroneCAN 节点名称（默认值）
#endif

#define UNUSED(x) (void)(x)             // 抑制未使用变量警告


// ==================== Canard 回调跳板函数声明 ====================
// 静态函数作为 Canard 库的回调入口，通过 user_reference 转发到 DroneCAN 实例
static void onTransferReceived_trampoline(CanardInstance* ins, CanardRxTransfer* transfer);
static bool shouldAcceptTransfer_trampoline(const CanardInstance* ins, uint64_t* out_data_type_signature, uint16_t data_type_id,
        CanardTransferType transfer_type,
        uint8_t source_node_id);

/*
  init() - DroneCAN 通信初始化
  1. 重置 DB210 板型的 CAN 引脚（该板型需要先释放 GPIO 复用）
  2. 配置 CAN 接收过滤器（仅接受优先级 >= 16 的消息，过滤掉 ESC 等高频消息）
  3. 初始化 Canard 协议栈和内存池
  4. 如果参数中配置了固定节点 ID 则直接设置，否则后续通过 DNA 自动分配
 */
void DroneCAN::init(void)
{

#if defined(BOARD_BLUEMARK_DB210)
    // DB210 板型：CAN 引脚（GPIO19/20）可能被 USB 复用，需先重置为普通 GPIO
    gpio_reset_pin(GPIO_NUM_19);
    gpio_reset_pin(GPIO_NUM_20);
#endif

    /*
      ESP32 的 CAN (TWAI) 协议栈效率较低。如果不加过滤地接收所有消息，
      在总线繁忙时 processRx() 会占用全部 CPU 时间。
      ESP32 只有一个简单的验收过滤器，无法精确地阻止 ESC 命令等高频消息
      而保留所有 RemoteID 消息。折中方案是利用消息优先级过滤：
      高频消息（如 ESC 命令）的优先级数值较小（即优先级高），
      通过在优先级字段最高位设置验收码，只接收优先级数值 >= 16 的消息
      （即 CANARD_TRANSFER_PRIORITY_MEDIUM 或更低优先级）。
    */
    const uint32_t acceptance_code = 0x10000000U<<3;  // 验收码：优先级字段最高位 = 1
    const uint32_t acceptance_mask = 0x0FFFFFFFU<<3;  // 验收掩码：只检查优先级最高位

    can_driver.init(1000000, acceptance_code, acceptance_mask);  // CAN 波特率 1Mbps

    // 初始化 Canard 协议栈：分配内存池，注册回调函数
    canardInit(&canard, (uint8_t *)canard_memory_pool, sizeof(canard_memory_pool),
               onTransferReceived_trampoline, shouldAcceptTransfer_trampoline, NULL);
    if (g.can_node > 0 && g.can_node < 128) {
        canardSetLocalNodeID(&canard, g.can_node);  // 使用参数中的固定节点 ID
    }
    canard.user_reference = (void*)this;  // 保存实例指针，供跳板函数回调
}

/*
  update() - DroneCAN 周期更新（在主循环中调用）
  1. 执行 DNA 流程（未分配节点 ID 时）
  2. 每秒发送一次节点状态和解锁状态广播
  3. 处理 CAN 帧的发送和接收
 */
void DroneCAN::update(void)
{
    if (do_DNA()) {
        const uint32_t now_ms = millis();
        if (now_ms - last_node_status_ms >= 1000) {
            last_node_status_ms = now_ms;
            node_status_send();
            arm_status_send();
        }
    }
    processTx();
    processRx();
}

/*
  node_status_send() - 发送 UAVCAN 节点状态广播
  以低优先级广播当前节点的运行时间和状态信息（1Hz 调用）
 */
void DroneCAN::node_status_send(void)
{
    uint8_t buffer[UAVCAN_PROTOCOL_NODESTATUS_MAX_SIZE];
    node_status.uptime_sec = millis() / 1000U;
    node_status.vendor_specific_status_code = 0;
    const uint16_t len = uavcan_protocol_NodeStatus_encode(&node_status, buffer);
    static uint8_t tx_id;

    canardBroadcast(&canard,
                    UAVCAN_PROTOCOL_NODESTATUS_SIGNATURE,
                    UAVCAN_PROTOCOL_NODESTATUS_ID,
                    &tx_id,
                    CANARD_TRANSFER_PRIORITY_LOW,
                    (void*)buffer,
                    len);
}

/*
  arm_status_send() - 发送 DroneCAN 解锁状态广播
  报告当前 RID 设备是否满足广播条件：
  - parse_fail == nullptr：数据验证通过，可以解锁（GOOD_TO_ARM）
  - parse_fail != nullptr：数据验证失败，附带失败原因字符串
 */
void DroneCAN::arm_status_send(void)
{
    uint8_t buffer[DRONECAN_REMOTEID_ARMSTATUS_MAX_SIZE];
    dronecan_remoteid_ArmStatus arm_status {};

    const uint8_t status = parse_fail==nullptr? MAV_ODID_ARM_STATUS_GOOD_TO_ARM:MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC;
    const char *reason = parse_fail==nullptr?"":parse_fail;

    arm_status.status = status;
    arm_status.error.len = strlen(reason);
    strncpy((char*)arm_status.error.data, reason, sizeof(arm_status.error.data));

    const uint16_t len = dronecan_remoteid_ArmStatus_encode(&arm_status, buffer);

    static uint8_t tx_id;
    canardBroadcast(&canard,
                    DRONECAN_REMOTEID_ARMSTATUS_SIGNATURE,
                    DRONECAN_REMOTEID_ARMSTATUS_ID,
                    &tx_id,
                    CANARD_TRANSFER_PRIORITY_LOW,
                    (void*)buffer,
                    len);
}

/*
  onTransferReceived() - Canard 收到完整传输后的分发处理
  根据消息类型 ID 分发到对应的处理函数：
  - 未分配节点 ID 时仅处理 DNA 分配响应
  - 已分配节点 ID 后处理所有 RID 消息、参数请求、安全命令等
 */
void DroneCAN::onTransferReceived(CanardInstance* ins,
                                  CanardRxTransfer* transfer)
{
    // 未分配节点 ID 时只处理 DNA 分配响应
    if (canardGetLocalNodeID(ins) == CANARD_BROADCAST_NODE_ID) {
        if (transfer->transfer_type == CanardTransferTypeBroadcast &&
                transfer->data_type_id == UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_ID) {
            handle_allocation_response(ins, transfer);
        }
        return;
    }

    // 已分配节点 ID：按消息类型分发处理
    const uint32_t now_ms = millis();

    switch (transfer->data_type_id) {
    case UAVCAN_PROTOCOL_GETNODEINFO_ID:        // 节点信息查询请求
        handle_get_node_info(ins, transfer);
        break;
    case UAVCAN_PROTOCOL_RESTARTNODE_ID:         // 节点重启请求
        Serial.printf("DroneCAN: restartNode\n");
        delay(20);
        esp_restart();
        break;
    case DRONECAN_REMOTEID_BASICID_ID:           // 基本标识消息
        Serial.printf("DroneCAN: got BasicID\n");
        handle_BasicID(transfer);
        break;
    case DRONECAN_REMOTEID_LOCATION_ID:          // 位置信息消息
        Serial.printf("DroneCAN: got Location\n");
        handle_Location(transfer);
        break;
    case DRONECAN_REMOTEID_SELFID_ID:            // 自我标识消息
        Serial.printf("DroneCAN: got SelfID\n");
        handle_SelfID(transfer);
        break;
    case DRONECAN_REMOTEID_SYSTEM_ID:            // 系统信息消息
        Serial.printf("DroneCAN: got System\n");
        handle_System(transfer);
        break;
    case DRONECAN_REMOTEID_OPERATORID_ID:        // 操作员标识消息
        Serial.printf("DroneCAN: got OperatorID\n");
        handle_OperatorID(transfer);
        break;
    case UAVCAN_PROTOCOL_PARAM_GETSET_ID:        // 参数读写请求
        handle_param_getset(ins, transfer);
        break;
    case DRONECAN_REMOTEID_SECURECOMMAND_ID:     // 安全命令请求
        handle_SecureCommand(ins, transfer);
        break;
    default:
        //Serial.printf("reject %u\n", transfer->data_type_id);
        break;
    }
}

/*
  shouldAcceptTransfer() - 判断是否接受指定的 CAN 传输
  Canard 在收到新传输时调用此函数，通过消息类型 ID 判断是否需要处理。
  只接受 DNA 分配、节点信息、重启、RID 消息、参数操作和安全命令。
  使用 ACCEPT_ID 宏简化签名匹配。
 */
bool DroneCAN::shouldAcceptTransfer(const CanardInstance* ins,
                                    uint64_t* out_data_type_signature,
                                    uint16_t data_type_id,
                                    CanardTransferType transfer_type,
                                    uint8_t source_node_id)
{
    // DNA 分配阶段：未获取节点 ID 时只接受分配响应
    if (canardGetLocalNodeID(ins) == CANARD_BROADCAST_NODE_ID &&
            data_type_id == UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_ID) {
        *out_data_type_signature = UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_SIGNATURE;
        return true;
    }

// 接受指定消息类型的辅助宏：匹配 ID 并填入对应的数据类型签名
#define ACCEPT_ID(name) case name ## _ID: *out_data_type_signature = name ## _SIGNATURE; return true
    switch (data_type_id) {
        ACCEPT_ID(UAVCAN_PROTOCOL_GETNODEINFO);       // 节点信息查询
        ACCEPT_ID(UAVCAN_PROTOCOL_RESTARTNODE);        // 节点重启
        ACCEPT_ID(DRONECAN_REMOTEID_BASICID);          // 基本标识
        ACCEPT_ID(DRONECAN_REMOTEID_LOCATION);         // 位置
        ACCEPT_ID(DRONECAN_REMOTEID_SELFID);           // 自我标识
        ACCEPT_ID(DRONECAN_REMOTEID_OPERATORID);       // 操作员标识
        ACCEPT_ID(DRONECAN_REMOTEID_SYSTEM);           // 系统信息
        ACCEPT_ID(DRONECAN_REMOTEID_SECURECOMMAND);    // 安全命令
        ACCEPT_ID(UAVCAN_PROTOCOL_PARAM_GETSET);       // 参数读写
        return true;
    }
    //Serial.printf("%u: reject ID 0x%x\n", millis(), data_type_id);
    return false;
}

// onTransferReceived 跳板函数：从 Canard 的 C 回调转发到 DroneCAN C++ 实例
static void onTransferReceived_trampoline(CanardInstance* ins,
        CanardRxTransfer* transfer)
{
    DroneCAN *dc = (DroneCAN *)ins->user_reference;
    dc->onTransferReceived(ins, transfer);
}

// shouldAcceptTransfer 跳板函数：判断是否接受该传输
static bool shouldAcceptTransfer_trampoline(const CanardInstance* ins,
        uint64_t* out_data_type_signature,
        uint16_t data_type_id,
        CanardTransferType transfer_type,
        uint8_t source_node_id)
{
    DroneCAN *dc = (DroneCAN *)ins->user_reference;
    return dc->shouldAcceptTransfer(ins, out_data_type_signature,
                                    data_type_id,
                                    transfer_type,
                                    source_node_id);
}

/*
  processTx() - 处理 CAN 发送队列
  从 Canard 发送队列中取出帧，通过 CAN 驱动发送到总线。
  发送失败时重试，连续失败超过 8 次则丢弃当前帧（防止队列阻塞）。
 */
void DroneCAN::processTx(void)
{
    for (const CanardCANFrame* txf = NULL; (txf = canardPeekTxQueue(&canard)) != NULL;) {
        CANFrame txmsg {};
        txmsg.dlc = CANFrame::dataLengthToDlc(txf->data_len);  // 数据长度转 DLC
        memcpy(txmsg.data, txf->data, txf->data_len);
        txmsg.id = (txf->id | CANFrame::FlagEFF);               // 设置扩展帧标志

        // 尝试发送
        if (can_driver.send(txmsg)) {
            canardPopTxQueue(&canard);  // 发送成功，移除队列头
            tx_fail_count = 0;
        } else {
            if (tx_fail_count < 8) {
                tx_fail_count++;        // 失败计数 +1，下次重试
            } else {
                canardPopTxQueue(&canard);  // 连续失败 8 次，丢弃该帧
            }
            break;
        }
    }
}

/*
  processRx() - 处理 CAN 接收
  从 CAN 驱动读取帧（每次最多 60 帧），交给 Canard 协议栈解析。
  Canard 内部完成多帧重组后会触发 onTransferReceived 回调。
 */
void DroneCAN::processRx(void)
{
    CANFrame rxmsg;
    uint8_t count = 60;
    while (count-- && can_driver.receive(rxmsg)) {
        CanardCANFrame rx_frame {};
        uint64_t timestamp = micros64();
        rx_frame.data_len = CANFrame::dlcToDataLength(rxmsg.dlc);
        memcpy(rx_frame.data, rxmsg.data, rx_frame.data_len);
        rx_frame.id = rxmsg.id;
        int err = canardHandleRxFrame(&canard, &rx_frame, timestamp);
#if 0
        Serial.printf("%u: FX %08x %02x %02x %02x %02x %02x %02x %02x %02x (%u) -> %d\n",
                      millis(),
                      rx_frame.id,
                      rxmsg.data[0], rxmsg.data[1], rxmsg.data[2], rxmsg.data[3],
                      rxmsg.data[4], rxmsg.data[5], rxmsg.data[6], rxmsg.data[7],
                      rx_frame.data_len,
                      err);
#else
        UNUSED(err);
#endif
    }
}

// ==================== CANFrame 类实现 ====================

/*
  CANFrame 构造函数
  从原始 CAN ID 和数据构造帧对象，DLC 限制为 8（经典 CAN 最大值）
 */
CANFrame::CANFrame(uint32_t can_id, const uint8_t* can_data, uint8_t data_len, bool canfd_frame) :
    id(can_id)
{
    if ((can_data == nullptr) || (data_len == 0) || (data_len > MaxDataLen)) {
        return;
    }
    memcpy(this->data, can_data, data_len);
    if (data_len <= 8) {
        dlc = data_len;
    } else {
        dlc = 8;
    }
}

/*
  dataLengthToDlc() - 数据长度转 DLC 编码
  CAN FD 中 DLC > 8 时采用非线性映射（9→12, 10→16, ... 15→64 字节）
 */
uint8_t CANFrame::dataLengthToDlc(uint8_t data_length)
{
    if (data_length <= 8) {
        return data_length;
    } else if (data_length <= 12) {
        return 9;
    } else if (data_length <= 16) {
        return 10;
    } else if (data_length <= 20) {
        return 11;
    } else if (data_length <= 24) {
        return 12;
    } else if (data_length <= 32) {
        return 13;
    } else if (data_length <= 48) {
        return 14;
    }
    return 15;
}


/*
  dlcToDataLength() - DLC 编码转实际数据长度
  DLC 编码对照表：
    DLC:    9   10   11   12   13   14   15
    字节:  12   16   20   24   32   48   64
 */
uint8_t CANFrame::dlcToDataLength(uint8_t dlc)
{
    /*
    Data Length Code      9  10  11  12  13  14  15
    Number of data bytes 12  16  20  24  32  48  64
    */
    if (dlc <= 8) {
        return dlc;
    } else if (dlc == 9) {
        return 12;
    } else if (dlc == 10) {
        return 16;
    } else if (dlc == 11) {
        return 20;
    } else if (dlc == 12) {
        return 24;
    } else if (dlc == 13) {
        return 32;
    } else if (dlc == 14) {
        return 48;
    }
    return 64;
}

/*
  micros64() - 获取 64 位微秒时间戳
  ESP32 的 micros() 返回 32 位值，约 71 分钟溢出一次。
  通过检测 32 位值回绕并累加高位基准，扩展为 64 位时间戳。
 */
uint64_t DroneCAN::micros64(void)
{
    uint32_t us = micros();
    if (us < last_micros32) {
        base_micros64 += 0x100000000ULL;
    }
    last_micros32 = us;
    return us + base_micros64;
}

/*
  handle_get_node_info() - 处理 GetNodeInfo 请求
  返回节点的软硬件版本、唯一 ID 和名称等信息，
  供 DroneCAN 工具（如 DroneCAN GUI Tool）识别设备。
 */
void DroneCAN::handle_get_node_info(CanardInstance* ins, CanardRxTransfer* transfer)
{
    uint8_t buffer[UAVCAN_PROTOCOL_GETNODEINFO_RESPONSE_MAX_SIZE] {};
    uavcan_protocol_GetNodeInfoResponse pkt {};

    node_status.uptime_sec = millis() / 1000U;

    pkt.status = node_status;                 // 当前节点状态
    pkt.software_version.major = FW_VERSION_MAJOR;  // 固件主版本号
    pkt.software_version.minor = FW_VERSION_MINOR;  // 固件次版本号
    pkt.software_version.optional_field_flags = UAVCAN_PROTOCOL_SOFTWAREVERSION_OPTIONAL_FIELD_FLAG_VCS_COMMIT | UAVCAN_PROTOCOL_SOFTWAREVERSION_OPTIONAL_FIELD_FLAG_IMAGE_CRC;
    pkt.software_version.vcs_commit = GIT_VERSION;  // Git 提交哈希

    readUniqueID(pkt.hardware_version.unique_id);   // ESP32 唯一 ID（eFuse MAC）

    pkt.hardware_version.major = CAN_BOARD_ID >> 8;    // 板 ID 高字节
    pkt.hardware_version.minor = CAN_BOARD_ID & 0xFF;  // 板 ID 低字节
    snprintf((char*)pkt.name.data, sizeof(pkt.name.data), "%s", CAN_APP_NODE_NAME);  // 节点名称
    pkt.name.len = strnlen((char*)pkt.name.data, sizeof(pkt.name.data));

    uint16_t total_size = uavcan_protocol_GetNodeInfoResponse_encode(&pkt, buffer);
    canardRequestOrRespond(ins,
                           transfer->source_node_id,
                           UAVCAN_PROTOCOL_GETNODEINFO_SIGNATURE,
                           UAVCAN_PROTOCOL_GETNODEINFO_ID,
                           &transfer->transfer_id,
                           transfer->priority,
                           CanardResponse,
                           &buffer[0],
                           total_size);
}

/*
  handle_allocation_response() - 处理动态节点 ID 分配响应
  DNA 协议流程：
  1. 节点发送包含部分 UID 的分配请求
  2. 分配器回复已确认的 UID 前缀
  3. 节点继续发送剩余 UID 部分
  4. 当分配器确认完整 UID 后，返回分配的节点 ID
  遵循 UAVCAN 规范中的 Rule C（收到响应后重置随机延时）
 */
void DroneCAN::handle_allocation_response(CanardInstance* ins, CanardRxTransfer* transfer)
{
    // Rule C：收到分配响应后更新随机延时（避免多节点冲突）
    send_next_node_id_allocation_request_at_ms =
        millis() + UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_MIN_REQUEST_PERIOD_MS +
        random(1, UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_MAX_FOLLOWUP_DELAY_MS);

    if (transfer->source_node_id == CANARD_BROADCAST_NODE_ID) {
        node_id_allocation_unique_id_offset = 0;  // 广播源：重置 UID 偏移
        return;
    }

    // 解码分配响应消息
    uavcan_protocol_dynamic_node_id_Allocation msg;

    uavcan_protocol_dynamic_node_id_Allocation_decode(transfer, &msg);

    // 获取本地唯一 ID（ESP32 eFuse MAC 地址）
    uint8_t my_unique_id[sizeof(msg.unique_id.data)] {};
    readUniqueID(my_unique_id);

    // 比较响应中的 UID 与本地 UID 是否匹配
    if (memcmp(msg.unique_id.data, my_unique_id, msg.unique_id.len) != 0) {
        node_id_allocation_unique_id_offset = 0;  // UID 不匹配，重置
        return;
    }

    if (msg.unique_id.len < sizeof(msg.unique_id.data)) {
        // 分配器已确认部分 UID，切换到下一阶段继续发送剩余部分
        node_id_allocation_unique_id_offset = msg.unique_id.len;
        send_next_node_id_allocation_request_at_ms -= UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_MIN_REQUEST_PERIOD_MS;
    } else {
        // UID 完全确认，分配完成 — 从消息中提取分配的节点 ID
        canardSetLocalNodeID(ins, msg.node_id);
        Serial.printf("Node ID allocated: %u\n", unsigned(msg.node_id));
    }
}

/*
  do_DNA() - 执行动态节点 ID 分配流程
  如果已有节点 ID 则直接返回 true。
  否则构造包含本地 UID 的分配请求并广播。
  每次最多发送 6 字节 UID，首次请求设置 first_part_of_unique_id 标志位。
  返回值：true = 已获取节点 ID，false = 仍在分配中
 */
bool DroneCAN::do_DNA(void)
{
    if (canardGetLocalNodeID(&canard) != CANARD_BROADCAST_NODE_ID) {
        return true;  // 已有节点 ID
    }
    const uint32_t now = millis();
    if (now - last_DNA_start_ms < 1000 && node_id_allocation_unique_id_offset == 0) {
        return false;  // 冷却期内且未开始新阶段，跳过
    }
    last_DNA_start_ms = now;

    uint8_t node_id_allocation_transfer_id = 0;
    UNUSED(node_id_allocation_transfer_id);
    send_next_node_id_allocation_request_at_ms =
        now + UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_MIN_REQUEST_PERIOD_MS +
        random(1, UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_MAX_FOLLOWUP_DELAY_MS);

    // 构造分配请求
    uint8_t allocation_request[CANARD_CAN_FRAME_MAX_DATA_LEN - 1] {};
    allocation_request[0] = 0;
    if (node_id_allocation_unique_id_offset == 0) {
        allocation_request[0] |= 1;  // 首次请求：设置 first_part_of_unique_id 标志
    }

    // 从本地 UID 中提取待发送部分（每次最多 6 字节）
    uint8_t my_unique_id[sizeof(uavcan_protocol_dynamic_node_id_Allocation::unique_id.data)] {};
    readUniqueID(my_unique_id);

    static const uint8_t MaxLenOfUniqueIDInRequest = 6;
    uint8_t uid_size = (uint8_t)(sizeof(uavcan_protocol_dynamic_node_id_Allocation::unique_id.data) - node_id_allocation_unique_id_offset);

    if (uid_size > MaxLenOfUniqueIDInRequest) {
        uid_size = MaxLenOfUniqueIDInRequest;
    }

    memmove(&allocation_request[1], &my_unique_id[node_id_allocation_unique_id_offset], uid_size);

    // 广播分配请求
    static uint8_t tx_id;
    canardBroadcast(&canard,
                    UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_SIGNATURE,
                    UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_ALLOCATION_ID,
                    &tx_id,
                    CANARD_TRANSFER_PRIORITY_LOW,
                    &allocation_request[0],
                    (uint16_t) (uid_size + 1));
    node_id_allocation_unique_id_offset = 0;
    return false;
}

/*
  readUniqueID() - 读取 ESP32 唯一设备 ID
  使用 ESP32 eFuse 中的出厂 MAC 地址作为 6 字节唯一标识
 */
void DroneCAN::readUniqueID(uint8_t id[6])
{
    esp_efuse_mac_get_default(id);
}

// ==================== RID 消息处理辅助宏 ====================
#define IMIN(a,b) ((a)<(b)?(a):(b))                                                   // 取较小值
#define COPY_FIELD(fname) mpkt.fname = pkt.fname                                       // 复制同名字段
#define COPY_STR(fname) memcpy(mpkt.fname, pkt.fname.data, IMIN(pkt.fname.len, sizeof(mpkt.fname)))  // 复制字符串字段

/*
  handle_BasicID() - 处理 DroneCAN 基本标识消息
  仅当收到有效数据（非空 UAS ID 且 ID 类型合法）时才更新本地数据
 */
void DroneCAN::handle_BasicID(CanardRxTransfer* transfer)
{
    dronecan_remoteid_BasicID pkt {};
    dronecan_remoteid_BasicID_decode(transfer, &pkt);

    if ((pkt.uas_id.len > 0) && (pkt.id_type > 0) && (pkt.id_type <= MAV_ODID_ID_TYPE_SPECIFIC_SESSION_ID)) {
        // 仅接受有效数据：UAS ID 非空且 ID 类型在合法范围内
        auto &mpkt = basic_id;
        memset(&mpkt, 0, sizeof(mpkt));
        COPY_STR(id_or_mac);
        COPY_FIELD(id_type);
        COPY_FIELD(ua_type);
        COPY_STR(uas_id);
        last_basic_id_ms = millis();
    }
}

// handle_SelfID() - 处理 DroneCAN 自我标识消息（描述类型 + 文本描述）
void DroneCAN::handle_SelfID(CanardRxTransfer* transfer)
{
    dronecan_remoteid_SelfID pkt {};
    auto &mpkt = self_id;
    dronecan_remoteid_SelfID_decode(transfer, &pkt);
    last_self_id_ms = millis();
    memset(&mpkt, 0, sizeof(mpkt));
    COPY_STR(id_or_mac);
    COPY_FIELD(description_type);
    COPY_STR(description);
}

/*
  handle_System() - 处理 DroneCAN 系统信息消息
  包含操作员位置、区域信息、EU 分类等。
  仅当时间戳变化或为 0 时更新 last_system_ms（避免重复数据刷新超时计时器）。
 */
void DroneCAN::handle_System(CanardRxTransfer* transfer)
{
    dronecan_remoteid_System pkt {};
    auto &mpkt = system;
    dronecan_remoteid_System_decode(transfer, &pkt);

    // PX4 以 1Hz 持续广播 System，但消息里的时间戳字段可能长期不变（GPS UTC 未锁时），
    // 若只按时间戳变化刷新会误判为超时。只要收到消息就视为数据新鲜，刷新接收时间。
    last_system_ms = millis();
    last_system_timestamp = pkt.timestamp;
    memset(&mpkt, 0, sizeof(mpkt));

    COPY_STR(id_or_mac);
    COPY_FIELD(operator_location_type);
    COPY_FIELD(classification_type);
    COPY_FIELD(operator_latitude);
    COPY_FIELD(operator_longitude);
    COPY_FIELD(area_count);
    COPY_FIELD(area_radius);
    COPY_FIELD(area_ceiling);
    COPY_FIELD(area_floor);
    COPY_FIELD(category_eu);
    COPY_FIELD(class_eu);
    COPY_FIELD(operator_altitude_geo);
    COPY_FIELD(timestamp);
}

// handle_OperatorID() - 处理 DroneCAN 操作员标识消息
void DroneCAN::handle_OperatorID(CanardRxTransfer* transfer)
{
    dronecan_remoteid_OperatorID pkt {};
    auto &mpkt = operator_id;
    dronecan_remoteid_OperatorID_decode(transfer, &pkt);
    last_operator_id_ms = millis();
    memset(&mpkt, 0, sizeof(mpkt));

    COPY_STR(id_or_mac);
    COPY_FIELD(operator_id_type);
    COPY_STR(operator_id);
}

/*
  handle_Location() - 处理 DroneCAN 位置信息消息
  包含经纬度、高度、速度、精度等飞行器位置数据。
  仅当时间戳变化时更新接收时间。
 */
void DroneCAN::handle_Location(CanardRxTransfer* transfer)
{
    dronecan_remoteid_Location pkt {};
    auto &mpkt = location;
    dronecan_remoteid_Location_decode(transfer, &pkt);
    if (last_location_timestamp != pkt.timestamp) {
        // 仅当时间戳不同时更新接收时间
        last_location_ms = millis();
        last_location_timestamp = pkt.timestamp;
    }
    memset(&mpkt, 0, sizeof(mpkt));

    COPY_STR(id_or_mac);
    COPY_FIELD(status);
    COPY_FIELD(direction);
    COPY_FIELD(speed_horizontal);
    COPY_FIELD(speed_vertical);
    COPY_FIELD(latitude);
    COPY_FIELD(longitude);
    COPY_FIELD(altitude_barometric);
    COPY_FIELD(altitude_geodetic);
    COPY_FIELD(height_reference);
    COPY_FIELD(height);
    COPY_FIELD(horizontal_accuracy);
    COPY_FIELD(vertical_accuracy);
    COPY_FIELD(barometer_accuracy);
    COPY_FIELD(speed_accuracy);
    COPY_FIELD(timestamp);
    COPY_FIELD(timestamp_accuracy);
}

/*
  handle_param_getset() - 处理参数读写请求（GetSet 服务）
  支持按名称或索引查找参数，支持以下参数类型：
  - UINT8/INT8/UINT32：整数值
  - FLOAT：浮点值
  - CHAR20/CHAR64：字符串值
  参数锁定时（lock_level > 0）拒绝写入操作。
  密码类型参数在读取时返回 "********" 掩码。
 */
void DroneCAN::handle_param_getset(CanardInstance* ins, CanardRxTransfer* transfer)
{
    uavcan_protocol_param_GetSetRequest req;
    if (uavcan_protocol_param_GetSetRequest_decode(transfer, &req)) {
        return;
    }

    uavcan_protocol_param_GetSetResponse pkt {};

    const Parameters::Param *vp = nullptr;

    // 查找参数：优先按名称查找，名称为空时按索引查找
    if (req.name.len != 0 && req.name.len > PARAM_NAME_MAX_LEN) {
        vp = nullptr;  // 名称过长，无效
    } else if (req.name.len != 0 && req.name.len <= PARAM_NAME_MAX_LEN) {
        memcpy((char *)pkt.name.data, (char *)req.name.data, req.name.len);
        vp = Parameters::find((char *)pkt.name.data);
    } else {
        vp = Parameters::find_by_index(req.index);
    }
    if (vp != nullptr && (vp->flags & PARAM_FLAG_HIDDEN)) {
        vp = nullptr;  // 隐藏参数不可访问
    }
    if (vp != nullptr && req.name.len != 0 &&
        req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_EMPTY) {
        // 参数写入请求
        if (g.lock_level > 0) {
            can_printf("Parameters locked");  // 参数已锁定，拒绝写入
        } else {
            // 根据参数类型执行对应的写入操作
            switch (vp->ptype) {
            case Parameters::ParamType::UINT8:
                if (req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE) {
                    return;
                }
                vp->set_uint8(uint8_t(req.value.integer_value));
                break;
            case Parameters::ParamType::INT8:
                if (req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE) {
                    return;
                }
                vp->set_int8(int8_t(req.value.integer_value));
                break;
            case Parameters::ParamType::UINT32:
                if (req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE) {
                    return;
                }
                vp->set_uint32(uint32_t(req.value.integer_value));
                break;
            case Parameters::ParamType::FLOAT:
                if (req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_REAL_VALUE) {
                    return;
                }
                vp->set_float(req.value.real_value);
                break;
            case Parameters::ParamType::CHAR20: {
                if (req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_STRING_VALUE) {
                    return;
                }
                char v[21] {};
                strncpy(v, (const char *)&req.value.string_value.data[0], req.value.string_value.len);
                if (vp->min_len > 0 && strlen(v) < vp->min_len) {
                    can_printf("%s too short - min %u", vp->name, vp->min_len);
                } else {
                    vp->set_char20(v);
                }
                break;
            }
            case Parameters::ParamType::CHAR64: {
                if (req.value.union_tag != UAVCAN_PROTOCOL_PARAM_VALUE_STRING_VALUE) {
                    return;
                }
                char v[65] {};
                strncpy(v, (const char *)&req.value.string_value.data[0], req.value.string_value.len);
                vp->set_char64(v);
                break;
            }
            default:
                return;
            }
        }
    }
    if (vp != nullptr) {
        // 构造参数读取响应：填入当前值、默认值、最小/最大值
        switch (vp->ptype) {
        case Parameters::ParamType::UINT8:
            pkt.value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
            pkt.value.integer_value = vp->get_uint8();
            pkt.default_value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
            pkt.default_value.integer_value = uint8_t(vp->default_value);
            pkt.min_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
            pkt.min_value.integer_value = uint8_t(vp->min_value);
            pkt.max_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
            pkt.max_value.integer_value = uint8_t(vp->max_value);
            break;
        case Parameters::ParamType::INT8:
            pkt.value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
            pkt.value.integer_value = vp->get_int8();
            pkt.default_value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
            pkt.default_value.integer_value = int8_t(vp->default_value);
            pkt.min_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
            pkt.min_value.integer_value = int8_t(vp->min_value);
            pkt.max_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
            pkt.max_value.integer_value = int8_t(vp->max_value);
            break;
        case Parameters::ParamType::UINT32:
            pkt.value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
            pkt.value.integer_value = vp->get_uint32();
            pkt.default_value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_INTEGER_VALUE;
            pkt.default_value.integer_value = uint32_t(vp->default_value);
            pkt.min_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
            pkt.min_value.integer_value = uint32_t(vp->min_value);
            pkt.max_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_INTEGER_VALUE;
            pkt.max_value.integer_value = uint32_t(vp->max_value);
            break;
        case Parameters::ParamType::FLOAT:
            pkt.value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_REAL_VALUE;
            pkt.value.real_value = vp->get_float();
            pkt.default_value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_REAL_VALUE;
            pkt.default_value.real_value = vp->default_value;
            pkt.min_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_REAL_VALUE;
            pkt.min_value.real_value = vp->min_value;
            pkt.max_value.union_tag = UAVCAN_PROTOCOL_PARAM_NUMERICVALUE_REAL_VALUE;
            pkt.max_value.real_value = vp->max_value;
            break;
        case Parameters::ParamType::CHAR20: {
            pkt.value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_STRING_VALUE;
            const char *s = vp->get_char20();
            if (vp->flags & PARAM_FLAG_PASSWORD) {
                s = "********";  // 密码参数返回掩码
            }
            strncpy((char*)pkt.value.string_value.data, s, sizeof(pkt.value.string_value.data));
            pkt.value.string_value.len = strlen(s);
            break;
        }
        case Parameters::ParamType::CHAR64: {
            pkt.value.union_tag = UAVCAN_PROTOCOL_PARAM_VALUE_STRING_VALUE;
            const char *s = vp->get_char64();
            strncpy((char*)pkt.value.string_value.data, s, sizeof(pkt.value.string_value.data));
            pkt.value.string_value.len = strlen(s);
            break;
        }
        default:
            return;
        }
        pkt.name.len = strlen(vp->name);
        strncpy((char *)pkt.name.data, vp->name, sizeof(pkt.name.data));
    }

    // 编码并发送 GetSet 响应
    uint8_t buffer[UAVCAN_PROTOCOL_PARAM_GETSET_RESPONSE_MAX_SIZE] {};
    uint16_t total_size = uavcan_protocol_param_GetSetResponse_encode(&pkt, buffer);

    canardRequestOrRespond(ins,
                           transfer->source_node_id,
                           UAVCAN_PROTOCOL_PARAM_GETSET_SIGNATURE,
                           UAVCAN_PROTOCOL_PARAM_GETSET_ID,
                           &transfer->transfer_id,
                           transfer->priority,
                           CanardResponse,
                           &buffer[0],
                           total_size);
}

/*
  handle_SecureCommand() - 处理安全命令请求
  安全命令需要 Ed25519 签名验证（由 Transport 基类的 check_signature 执行）。
  支持的命令：
  - GET_REMOTEID_SESSION_KEY：生成并返回会话密钥
  - SET_REMOTEID_CONFIG：通过 NUL 分隔的 "NAME=VALUE" 对设置配置参数
 */
void DroneCAN::handle_SecureCommand(CanardInstance* ins, CanardRxTransfer* transfer)
{
    dronecan_remoteid_SecureCommandRequest req;
    if (dronecan_remoteid_SecureCommandRequest_decode(transfer, &req)) {
        return;
    }

    dronecan_remoteid_SecureCommandResponse reply {};
    reply.result = DRONECAN_REMOTEID_SECURECOMMAND_RESPONSE_RESULT_UNSUPPORTED;  // 默认：不支持
    reply.sequence = req.sequence;
    reply.operation = req.operation;

    // 验证 Ed25519 签名
    if (!check_signature(req.sig_length, req.data.len-req.sig_length,
                         req.sequence, req.operation, req.data.data)) {
        reply.result = DRONECAN_REMOTEID_SECURECOMMAND_RESPONSE_RESULT_DENIED;  // 签名验证失败
        goto send_reply;
    }

    switch (req.operation) {
    case DRONECAN_REMOTEID_SECURECOMMAND_REQUEST_SECURE_COMMAND_GET_REMOTEID_SESSION_KEY: {
        // 生成会话密钥并返回
        make_session_key(session_key);
        memcpy(reply.data.data, session_key, sizeof(session_key));
        reply.data.len = sizeof(session_key);
        reply.result = DRONECAN_REMOTEID_SECURECOMMAND_RESPONSE_RESULT_ACCEPTED;
        break;
    }
    case DRONECAN_REMOTEID_SECURECOMMAND_REQUEST_SECURE_COMMAND_SET_REMOTEID_CONFIG: {
        // 解析并设置配置参数
        Serial.printf("SECURE_COMMAND_SET_REMOTEID_CONFIG\n");
        int16_t data_len = req.data.len - req.sig_length;
        req.data.data[data_len] = 0;
        // 命令缓冲区是 NUL 分隔的 "NAME=VALUE" 对
        reply.result = DRONECAN_REMOTEID_SECURECOMMAND_RESPONSE_RESULT_ACCEPTED;
        char *command = (char *)req.data.data;
        while (data_len > 0) {
            uint8_t cmdlen = strlen(command);
            Serial.printf("set_config %s", command);
            char *eq = strchr(command, '=');  // 查找 '=' 分隔符
            if (eq != nullptr) {
                *eq = 0;  // 将 '=' 替换为 NUL，分离参数名和值
                if (!g.set_by_name_string(command, eq+1)) {
                    reply.result = DRONECAN_REMOTEID_SECURECOMMAND_RESPONSE_RESULT_FAILED;  // 设置失败
                }
            }
            command += cmdlen+1;
            data_len -= cmdlen+1;
        }
        break;
    }
    }

send_reply:
    // 编码并发送安全命令响应
    uint8_t buffer[UAVCAN_PROTOCOL_PARAM_GETSET_RESPONSE_MAX_SIZE] {};
    uint16_t total_size = dronecan_remoteid_SecureCommandResponse_encode(&reply, buffer);

    canardRequestOrRespond(ins,
                           transfer->source_node_id,
                           DRONECAN_REMOTEID_SECURECOMMAND_SIGNATURE,
                           DRONECAN_REMOTEID_SECURECOMMAND_ID,
                           &transfer->transfer_id,
                           transfer->priority,
                           CanardResponse,
                           &buffer[0],
                           total_size);
}

// can_printf() - 通过 CAN LogMessage 发送调试信息
// 将格式化字符串封装为 UAVCAN LogMessage 广播到 CAN 总线
void DroneCAN::can_printf(const char *fmt, ...)
{
    uavcan_protocol_debug_LogMessage pkt {};
    uint8_t buffer[UAVCAN_PROTOCOL_DEBUG_LOGMESSAGE_MAX_SIZE] {};
    va_list ap;
    va_start(ap, fmt);
    uint32_t n = vsnprintf((char*)pkt.text.data, sizeof(pkt.text.data), fmt, ap);
    va_end(ap);
    pkt.text.len = n;
    if (sizeof(pkt.text.data) < n) {
        pkt.text.len = sizeof(pkt.text.data);
    }

    uint32_t len = uavcan_protocol_debug_LogMessage_encode(&pkt, buffer);
    static uint8_t tx_id;

    canardBroadcast(&canard,
                    UAVCAN_PROTOCOL_DEBUG_LOGMESSAGE_SIGNATURE,
                    UAVCAN_PROTOCOL_DEBUG_LOGMESSAGE_ID,
                    &tx_id,
                    CANARD_TRANSFER_PRIORITY_LOW,
                    buffer,
                    len);
}



// xprintf 调试辅助函数（仅在调试 libcanard 等 C 代码时启用，正常编译时禁用）
#if 0
// xprintf is useful when debugging in C code such as libcanard
extern "C" {
    void xprintf(const char *fmt, ...);
}

void xprintf(const char *fmt, ...)
{
    char buffer[200] {};
    va_list ap;
    va_start(ap, fmt);
    uint32_t n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    Serial.printf("%s", buffer);
}
#endif
