/*
  CAN 总线底层驱动模块

  封装 ESP32 TWAI（Two-Wire Automotive Interface）外设，
  提供 CAN 2.0B 帧的发送和接收接口。

  主要功能：
  - 根据目标波特率自动计算时序参数（prescaler/BS1/BS2/SJW）
  - 支持接收过滤器（acceptance_code/mask）
  - 总线错误自动恢复
 */

struct CANFrame;    // 前向声明

// CAN 驱动类
class CANDriver {
public:
    CANDriver();

    // 初始化 CAN 外设：设置波特率和接收过滤器
    void init(uint32_t bitrate, uint32_t acceptance_code, uint32_t acceptance_mask);

    bool send(const CANFrame &frame);       // 发送一帧 CAN 数据
    bool receive(CANFrame &out_frame);      // 接收一帧 CAN 数据（非阻塞）

private:
    // CAN 总线时序参数
    struct Timings {
        uint16_t prescaler;     // 时钟预分频值
        uint8_t sjw;            // 同步跳转宽度 (Synchronization Jump Width)
        uint8_t bs1;            // 位段 1（传播段 + 相位段 1）
        uint8_t bs2;            // 位段 2（相位段 2）

        Timings()
            : prescaler(0)
            , sjw(0)
            , bs1(0)
            , bs2(0)
        { }
    };

    bool init_bus(const uint32_t bitrate);   // 初始化 TWAI 外设
    void init_once(bool enable_irq);         // 仅首次调用的初始化
    bool computeTimings(uint32_t target_bitrate, Timings& out_timings);  // 计算时序参数

    uint32_t bitrate;                       // 当前波特率
    uint32_t last_bus_recovery_ms;          // 上次总线恢复的时间戳
};

/*
  CAN 帧结构

  封装原始 CAN 帧数据，支持标准帧（11位ID）和扩展帧（29位ID）。
  DroneCAN 使用扩展帧格式。
 */
struct CANFrame {
    // ==================== CAN ID 标志位 ====================
    static const uint32_t MaskStdID = 0x000007FFU;      // 标准帧 ID 掩码（11位）
    static const uint32_t MaskExtID = 0x1FFFFFFFU;      // 扩展帧 ID 掩码（29位）
    static const uint32_t FlagEFF = 1U << 31;           // 扩展帧格式标志
    static const uint32_t FlagRTR = 1U << 30;           // 远程传输请求标志
    static const uint32_t FlagERR = 1U << 29;           // 错误帧标志

    // ==================== 数据长度限制 ====================
    static const uint8_t NonFDCANMaxDataLen = 8;        // 经典 CAN 最大数据长度
    static const uint8_t MaxDataLen = 8;                // 当前最大数据长度（不支持 CAN FD）

    // ==================== 帧数据 ====================
    uint32_t id;                // CAN ID（含标志位）
    union {
        uint8_t data[MaxDataLen];       // 按字节访问数据
        uint32_t data_32[MaxDataLen/4]; // 按 32 位字访问数据
    };
    uint8_t dlc;                // 数据长度码 (Data Length Code)

    // 默认构造函数
    CANFrame() :
        id(0),
        dlc(0)
    {
        memset(data,0, MaxDataLen);
    }

    // 从 CAN ID 和数据构造帧
    CANFrame(uint32_t can_id, const uint8_t* can_data, uint8_t data_len, bool canfd_frame = false);

    // ==================== 比较运算符 ====================
    bool operator!=(const CANFrame& rhs) const
    {
        return !operator==(rhs);
    }
    bool operator==(const CANFrame& rhs) const
    {
        return (id == rhs.id) && (dlc == rhs.dlc) && (memcmp(data, rhs.data, dlc) == 0);
    }

    // ==================== 帧属性查询 ====================
    bool isExtended()                  const    // 是否为扩展帧
    {
        return id & FlagEFF;
    }
    bool isRemoteTransmissionRequest() const    // 是否为远程传输请求
    {
        return id & FlagRTR;
    }
    bool isErrorFrame()                const    // 是否为错误帧
    {
        return id & FlagERR;
    }

    // ==================== DLC 与数据长度转换 ====================
    static uint8_t dlcToDataLength(uint8_t dlc);        // DLC 转实际数据长度
    static uint8_t dataLengthToDlc(uint8_t data_length); // 数据长度转 DLC

    // CAN 帧优先级比较（仲裁规则）
    bool priorityHigherThan(const CANFrame& rhs) const;
    bool priorityLowerThan(const CANFrame& rhs) const
    {
        return rhs.priorityHigherThan(*this);
    }
};
