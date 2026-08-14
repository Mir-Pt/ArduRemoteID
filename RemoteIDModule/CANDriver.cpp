/*
  ESP32 CAN (TWAI) 底层驱动实现

  封装 ESP32 的 TWAI（Two-Wire Automotive Interface）外设，
  提供 CAN 2.0B 帧的发送和接收功能。

  主要功能：
  1. 初始化 TWAI 驱动（1Mbps，可配置接收过滤器）
  2. 帧发送（含总线状态检查和错误恢复）
  3. 帧接收（非阻塞，5ms 超时）
  4. 自动计算 CAN 时序参数（prescaler/BS1/BS2/SJW）

  总线错误处理：
  - BUS_OFF 状态下每 2 秒尝试一次恢复
  - STOPPED 状态下自动重新启动
 */
#include <Arduino.h>
#include "options.h"

#if AP_DRONECAN_ENABLED

#include "CANDriver.h"

// FreeRTOS 头文件（ESP32 TWAI 驱动依赖）
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/twai.h"
#include "board_config.h"

// CAN 中断处理函数名映射（ESP32 HAL 兼容）
#define CAN1_TX_IRQ_Handler      ESP32_CAN1_TX_HANDLER
#define CAN1_RX0_IRQ_Handler     ESP32_CAN1_RX0_HANDLER
#define CAN1_RX1_IRQ_Handler     ESP32_CAN1_RX1_HANDLER
#define CAN2_TX_IRQ_Handler      ESP32_CAN2_TX_HANDLER
#define CAN2_RX0_IRQ_Handler     ESP32_CAN2_RX0_HANDLER
#define CAN2_RX1_IRQ_Handler     ESP32_CAN2_RX1_HANDLER

// Canard CAN 帧标志位定义（与 canard.h 保持一致）
#define CANARD_CAN_FRAME_EFF                        (1UL << 31U)         // 扩展帧格式
#define CANARD_CAN_FRAME_RTR                        (1UL << 30U)         // 远程传输请求（UAVCAN 不使用）
#define CANARD_CAN_FRAME_ERR                        (1UL << 29U)         // 错误帧（UAVCAN 不使用）

// 构造函数
CANDriver::CANDriver()
{}

// TWAI 时序配置：1Mbps（使用 ESP-IDF 预定义宏）
static const twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
// TWAI 接收过滤器配置（初始为全部接受，在 init() 中修改）
static twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

/*
  init() - 初始化 CAN 驱动
  设置接收过滤器（单过滤器模式）并初始化 TWAI 外设。
 */
void CANDriver::init(uint32_t bitrate, uint32_t acceptance_code, uint32_t acceptance_mask)
{
    f_config.acceptance_code = acceptance_code;  // 验收码
    f_config.acceptance_mask = acceptance_mask;  // 验收掩码
    f_config.single_filter = true;               // 单过滤器模式（32位）
    init_bus(bitrate);
}

// TWAI 通用配置：正常模式，使用板级定义的 CAN TX/RX 引脚
// tx_queue_len=5：发送队列深度
// rx_queue_len=50：接收队列深度（需足够大以避免接收溢出）
// intr_flags=LEVEL2：中断优先级 2
static const twai_general_config_t g_config =                      {.mode = TWAI_MODE_NORMAL, .tx_io = PIN_CAN_TX, .rx_io = PIN_CAN_RX, \
                                                                    .clkout_io = TWAI_IO_UNUSED, .bus_off_io = TWAI_IO_UNUSED,      \
                                                                    .tx_queue_len = 5, .rx_queue_len = 50,                           \
                                                                    .alerts_enabled = TWAI_ALERT_NONE,  .clkout_divider = 0,        \
                                                                    .intr_flags = ESP_INTR_FLAG_LEVEL2
                                                                   };

/*
  init_once() - TWAI 驱动首次初始化
  1. 安装 TWAI 驱动（分配内部资源）
  2. 配置接收报警（数据到达和队列满报警）
  3. 启动 TWAI 驱动
 */
void CANDriver::init_once(bool enable_irq)
{
    // 安装 TWAI 驱动
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK)
    {
        Serial.printf("CAN/TWAI Driver installed\n");
    }
    else
    {
        Serial.printf("Failed to install CAN/TWAI driver\n");
        return;
    }

    // 配置报警：仅监听接收数据和接收队列满事件
    uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_RX_QUEUE_FULL;
    if (twai_reconfigure_alerts(alerts_to_enable, NULL) == ESP_OK) {
        Serial.printf("CAN/TWAI Alerts reconfigured\n");
    } else {
        Serial.printf("Failed to reconfigure CAN/TWAI alerts");
    }

    // 启动 TWAI 驱动
    if (twai_start() == ESP_OK)
    {
        Serial.printf("CAN/TWAI Driver started\n");
    }
    else
    {
        Serial.printf("Failed to start CAN/TWAI driver\n");
        return;
    }
}

/*
  init_bus() - 初始化 CAN 总线
  调用 init_once 完成 TWAI 驱动安装，然后计算并打印时序参数。
 */
bool CANDriver::init_bus(const uint32_t _bitrate)
{
    bitrate = _bitrate;
    init_once(true);

    Timings timings;
    if (!computeTimings(bitrate, timings)) {
        return false;
    }
    Serial.printf("Timings: presc=%u sjw=%u bs1=%u bs2=%u",
                  unsigned(timings.prescaler), unsigned(timings.sjw), unsigned(timings.bs1), unsigned(timings.bs2));
    return true;
}

/*
  computeTimings() - 计算 CAN 总线时序参数
  根据目标波特率计算最优的 prescaler、BS1、BS2 和 SJW 值。

  算法步骤：
  1. 计算 prescaler × BS 的乘积 = PCLK / 目标波特率
  2. 寻找使每位时间量子数最大的 prescaler（提高采样精度）
  3. 计算 BS1/BS2 使采样点尽可能接近 87.5%（CAN 推荐值）

  参考：U. Koppe, "Automatic Baudrate Detection in CANopen Networks"
  不同波特率的最优量子数/位：
  - 1000 kbps: 8-10 量子
  - 500/250/125 kbps: 16-17 量子
 */
bool CANDriver::computeTimings(uint32_t target_bitrate, Timings& out_timings)
{
    if (target_bitrate < 1) {
        return false;
    }

    /*
     * 硬件配置：PCLK 时钟频率（单位 Hz）
     */
    const uint32_t pclk = 100000;

    static const int MaxBS1 = 16;   // BS1 最大值
    static const int MaxBS2 = 8;    // BS2 最大值

    /*
     * 不同波特率的最优量子数/位（参考文献）：
     *   波特率          最优    最大
     *   1000 kbps      8       10
     *   500  kbps      16      17
     *   250  kbps      16      17
     *   125  kbps      16      17
     */
    const int max_quanta_per_bit = (target_bitrate >= 1000000) ? 10 : 17;

    static const int MaxSamplePointLocation = 900;  // 最大采样点位置（千分比，90%）

    /*
     * 计算 prescaler × BS（总时间量子数）：
     *   BITRATE = PCLK / (PRESCALER × (1 + BS1 + BS2))
     *   所以：PRESCALER × BS = PCLK / BITRATE
     */
    const uint32_t prescaler_bs = pclk / target_bitrate;

    /*
     * 寻找使量子数/位最大的 prescaler 值
     */
    uint8_t bs1_bs2_sum = uint8_t(max_quanta_per_bit - 1);

    while ((prescaler_bs % (1 + bs1_bs2_sum)) != 0) {
        if (bs1_bs2_sum <= 2) {
            return false;          // 无解
        }
        bs1_bs2_sum--;
    }

    const uint32_t prescaler = prescaler_bs / (1 + bs1_bs2_sum);
    if ((prescaler < 1U) || (prescaler > 1024U)) {
        return false;              // prescaler 超出硬件范围，无解
    }

    /*
     * 约束：BS1 + BS2 = bs1_bs2_sum
     * 目标：采样点尽可能接近 87.5%（推荐值）
     * 采样点 = (1 + BS1) / (1 + BS1 + BS2)
     *
     * 解方程 (1 + bs1) / (1 + bs1 + bs2) = 7/8 得：
     *   bs1 = (7 × bs1_bs2_sum - 1) / 8
     *
     * 准备两个方案（四舍五入和截断），取采样点更优的
     */
    struct BsPair {
        uint8_t bs1;
        uint8_t bs2;
        uint16_t sample_point_permill;

        BsPair() :
            bs1(0),
            bs2(0),
            sample_point_permill(0)
        { }

        BsPair(uint8_t bs1_bs2_sum, uint8_t arg_bs1) :
            bs1(arg_bs1),
            bs2(uint8_t(bs1_bs2_sum - bs1)),
            sample_point_permill(uint16_t(1000 * (1 + bs1) / (1 + bs1 + bs2)))
        {}

        bool isValid() const
        {
            return (bs1 >= 1) && (bs1 <= MaxBS1) && (bs2 >= 1) && (bs2 <= MaxBS2);
        }
    };

    // 方案一：四舍五入
    BsPair solution(bs1_bs2_sum, uint8_t(((7 * bs1_bs2_sum - 1) + 4) / 8));

    if (solution.sample_point_permill > MaxSamplePointLocation) {
        // 方案二：截断（采样点更靠前）
        solution = BsPair(bs1_bs2_sum, uint8_t((7 * bs1_bs2_sum - 1) / 8));
    }

    if ((target_bitrate != (pclk / (prescaler * (1 + solution.bs1 + solution.bs2)))) || !solution.isValid()) {
        return false;
    }

    Serial.printf("Timings: quanta/bit: %d, sample point location: %.1f%%",
                  int(1 + solution.bs1 + solution.bs2), float(solution.sample_point_permill) / 10.F);

    // 填入计算结果（寄存器值 = 实际值 - 1）
    out_timings.prescaler = uint16_t(prescaler - 1U);
    out_timings.sjw = 0;                                        // SJW = 1（寄存器值为 0）
    out_timings.bs1 = uint8_t(solution.bs1 - 1);
    out_timings.bs2 = uint8_t(solution.bs2 - 1);
    return true;
}

/*
  send() - 发送 CAN 帧
  将 CANFrame 转换为 TWAI 消息格式并发送。
  发送前检查总线状态：
  - STOPPED：自动重新启动
  - BUS_OFF：每 2 秒尝试恢复一次
  - RUNNING/RECOVERING：正常发送
  超时 5ms，返回是否发送成功。
 */
bool CANDriver::send(const CANFrame &frame)
{
    // 拒绝发送错误帧和超长帧
    if (frame.isErrorFrame() || frame.dlc > 8) {
        return false;
    }

    // 构造 TWAI 消息
    twai_message_t message {};
    message.identifier = frame.id;
    message.extd = frame.isExtended() ? 1 : 0;  // 扩展帧标志
    message.data_length_code = frame.dlc;
    memcpy(message.data, frame.data, 8);

    // 检查总线状态并处理异常
    twai_status_info_t info {};
    twai_get_status_info(&info);
    switch (info.state) {
    case TWAI_STATE_STOPPED:
        twai_start();       // 已停止：重新启动
        break;
    case TWAI_STATE_RUNNING:
    case TWAI_STATE_RECOVERING:
        break;              // 正常运行或恢复中：继续发送
    case TWAI_STATE_BUS_OFF: {
        // 总线关闭：每 2 秒尝试一次恢复
        uint32_t now = millis();
        if (now - last_bus_recovery_ms > 2000) {
            last_bus_recovery_ms = now;
            twai_initiate_recovery();
        }
        break;
    }
    }

    // 发送帧（5ms 超时）
    const esp_err_t sts = twai_transmit(&message, pdMS_TO_TICKS(5));
    if (sts == ESP_OK) {
        last_bus_recovery_ms = 0;   // 发送成功，重置恢复计时器
    }

    return (sts == ESP_OK);
}

/*
  receive() - 接收 CAN 帧（非阻塞）
  从 TWAI 接收队列中读取一帧数据（5ms 超时）。
  将 TWAI 消息格式转换为 CANFrame，自动设置扩展帧标志。
  错误帧返回 false。
 */
bool CANDriver::receive(CANFrame &out_frame)
{
    twai_message_t message {};
    esp_err_t recverr = twai_receive(&message, pdMS_TO_TICKS(5));  // 5ms 超时接收
    if (recverr != ESP_OK) {
        return false;   // 无数据或接收错误
    }

    memcpy(out_frame.data, message.data, 8);    // 复制数据
    out_frame.dlc = message.data_length_code;   // 数据长度码
    out_frame.id = message.identifier;          // CAN ID
    if (message.extd) {
        out_frame.id |= CANARD_CAN_FRAME_EFF;  // 设置扩展帧标志
    }
    if (out_frame.id & CANFrame::FlagERR) {     // 过滤错误帧
        return false;
    }
    return true;
}

#endif // AP_DRONECAN_ENABLED（DroneCAN 功能未启用时跳过整个文件）
