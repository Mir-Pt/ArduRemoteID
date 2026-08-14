/*
  DroneCAN 通信协议驱动头文件

  本模块通过 DroneCAN（基于 CAN 总线的无人机通信协议）接收飞控发来的
  OpenDroneID 远程识别数据，并提供节点管理功能。

  主要功能：
  1. 接收 DroneCAN RemoteID 消息（BasicID/Location/SelfID/System/OperatorID）
  2. 发送节点状态和解锁状态（NodeStatus/ArmStatus）
  3. 动态节点 ID 分配（DNA）
  4. 参数读写（GetSet）
  5. 安全命令处理（SecureCommand，带 Ed25519 签名验证）
 */

#include "CANDriver.h"
#include "transport.h"
#include <canard.h>

// DroneCAN 协议消息头文件（自动生成）
#include <canard.h>
#include <uavcan.protocol.NodeStatus.h>
#include <dronecan.remoteid.BasicID.h>
#include <dronecan.remoteid.Location.h>
#include <dronecan.remoteid.SelfID.h>
#include <dronecan.remoteid.System.h>
#include <dronecan.remoteid.OperatorID.h>
#include <dronecan.remoteid.SecureCommand.h>

#define CAN_POOL_SIZE 4096  // Canard 内存池大小（字节），用于 CAN 帧的收发缓冲


// DroneCAN 通信类，继承自 Transport 传输层基类
class DroneCAN : public Transport {
public:
    using Transport::Transport;
    void init(void) override;       // 初始化 CAN 硬件、Canard 协议栈和接收过滤器
    void update(void) override;     // 周期更新：执行 DNA、发送状态、收发 CAN 帧

private:
    uint32_t last_node_status_ms;   // 上次发送节点状态的时间戳
    CANDriver can_driver;           // ESP32 CAN (TWAI) 底层驱动
    CanardInstance canard;          // Canard 协议栈实例
    uint32_t canard_memory_pool[CAN_POOL_SIZE/sizeof(uint32_t)];  // Canard 内存池

    void node_status_send(void);    // 发送 UAVCAN 节点状态广播（1Hz）
    void arm_status_send(void);     // 发送 DroneCAN 解锁状态广播（1Hz）

    uint8_t tx_fail_count;          // 连续发送失败计数（超过 8 次丢弃当前帧）

    void processTx(void);           // 处理发送队列：将 Canard 帧发送到 CAN 总线
    void processRx(void);           // 处理接收：从 CAN 总线读取帧并交给 Canard 解析

    uint64_t micros64();            // 获取 64 位微秒时间戳（解决 32 位溢出问题）
    uint64_t base_micros64;         // 64 位时间戳高位基准
    uint32_t last_micros32;         // 上次 32 位时间戳（用于检测溢出）

    void handle_get_node_info(CanardInstance* ins, CanardRxTransfer* transfer);  // 处理节点信息请求

    void readUniqueID(uint8_t id[6]);  // 读取 ESP32 唯一 ID（eFuse MAC 地址）

    // ==================== 动态节点 ID 分配（DNA）====================
    bool do_DNA(void);              // 执行 DNA 流程（未分配 ID 时）
    void handle_allocation_response(CanardInstance* ins, CanardRxTransfer* transfer);  // 处理 DNA 响应

    uint32_t send_next_node_id_allocation_request_at_ms;  // 下次发送 DNA 请求的时间
    uint32_t node_id_allocation_unique_id_offset;          // DNA 中已确认的 UID 偏移量
    uint32_t last_DNA_start_ms;     // 上次 DNA 尝试开始时间

    uavcan_protocol_NodeStatus node_status;  // 节点状态数据

    // ==================== RID 消息处理 ====================
    void handle_BasicID(CanardRxTransfer* transfer);       // 处理基本标识消息
    void handle_SelfID(CanardRxTransfer* transfer);        // 处理自我标识消息
    void handle_OperatorID(CanardRxTransfer* transfer);    // 处理操作员标识消息
    void handle_System(CanardRxTransfer* transfer);        // 处理系统信息消息
    void handle_Location(CanardRxTransfer* transfer);      // 处理位置信息消息
    void handle_param_getset(CanardInstance* ins, CanardRxTransfer* transfer);    // 处理参数读写请求
    void handle_SecureCommand(CanardInstance* ins, CanardRxTransfer* transfer);   // 处理安全命令请求

    void can_printf(const char *fmt, ...);  // 通过 CAN LogMessage 发送调试信息

public:
    // Canard 回调函数（由静态跳板函数调用）
    void onTransferReceived(CanardInstance* ins, CanardRxTransfer* transfer);     // 收到传输时的分发处理
    bool shouldAcceptTransfer(const CanardInstance* ins,                          // 判断是否接受指定传输
                              uint64_t* out_data_type_signature,
                              uint16_t data_type_id,
                              CanardTransferType transfer_type,
                              uint8_t source_node_id);
};
