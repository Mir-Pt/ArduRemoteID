/*
  mavlink class for handling SECURE_COMMAND messages
 */

/*
  =====================================================================
  MAVLink 安全命令处理模块 (mavlink_secure_command.cpp)
  =====================================================================
  本文件实现了 MAVLink SECURE_COMMAND 消息的处理逻辑。

  SECURE_COMMAND 是一种经过 Ed25519 数字签名保护的命令协议，
  用于对设备执行敏感操作（如密钥管理、远程配置）。

  支持的操作类型：
    - GET_SESSION_KEY / GET_REMOTEID_SESSION_KEY：
        生成并返回会话密钥（session key），供后续加密通信使用
    - GET_PUBLIC_KEYS：
        按索引检索已存储的 Ed25519 公钥，返回指定范围内的公钥数据
    - SET_PUBLIC_KEYS：
        按索引写入一个或多个 Ed25519 公钥到设备存储
    - REMOVE_PUBLIC_KEYS：
        按索引删除已存储的公钥
    - SET_REMOTEID_CONFIG：
        通过 NUL 分隔的 "NAME=VALUE" 键值对设置设备配置参数

  安全机制：
    所有操作在执行前都必须通过 Ed25519 签名验证
    （调用 Transport 基类的 check_signature 方法），
    未通过验证的命令将被拒绝（返回 MAV_RESULT_DENIED）。

  数据包格式（mavlink_secure_command_t）：
    - data[] 字段同时承载有效载荷数据和签名数据
    - data_length 表示有效载荷的长度
    - sig_length 表示签名的长度
    - data_length + sig_length 不得超过 data[] 数组的总大小

  应答通过 mavlink_msg_secure_command_reply_send_struct() 发送，
  回复结构体中包含操作结果（result）、序列号（sequence）和应答数据。
  =====================================================================
*/

#include <Arduino.h>
#include "mavlink.h"
#include "board_config.h"
#include "version.h"
#include "parameters.h"

/*
  处理一条 SECURE_COMMAND（安全命令）消息。
  参数 pkt 是经 MAVLink 解码后的安全命令结构体引用，包含：
    - operation:    操作类型（枚举值，标识具体的安全命令）
    - sequence:     序列号，用于匹配请求与应答
    - data[]:       有效载荷数据 + 签名数据（共用同一缓冲区）
    - data_length:  有效载荷数据的字节长度
    - sig_length:   Ed25519 签名的字节长度
 */
void MAVLinkSerial::handle_secure_command(const mavlink_secure_command_t &pkt)
{
    /* 初始化应答结构体，零填充所有字段 */
    mavlink_secure_command_reply_t reply {};

    /* 默认结果设为"不支持"，后续各分支根据情况覆写 */
    reply.result = MAV_RESULT_UNSUPPORTED;

    /* 将请求的序列号和操作类型原样回传，便于发送方匹配应答 */
    reply.sequence = pkt.sequence;
    reply.operation = pkt.operation;

    /*
      数据长度合法性校验：
      data_length（有效载荷长度）+ sig_length（签名长度）之和
      不能超过 data[] 数组的总容量。
      使用 uint16_t 强制转换防止 uint8_t 加法溢出。
      若越界则直接拒绝，防止后续越界访问。
    */
    if (uint16_t(pkt.data_length) + uint16_t(pkt.sig_length) > sizeof(pkt.data)) {
        reply.result = MAV_RESULT_DENIED;
        goto send_reply;
    }

    /*
      Ed25519 签名验证：
      调用 Transport 基类提供的 check_signature() 方法，
      验证数据包中携带的数字签名是否合法。
      签名覆盖的范围包括：序列号、操作类型和有效载荷数据。
      验证失败则拒绝执行任何操作。
    */
    if (!check_signature(pkt.sig_length, pkt.data_length, pkt.sequence, pkt.operation, pkt.data)) {
        reply.result = MAV_RESULT_DENIED;
        goto send_reply;
    }

    /* 根据操作类型分发到对应的处理逻辑 */
    switch (pkt.operation) {

    /*
      ---------------------------------------------------------------
      获取会话密钥操作：
      GET_SESSION_KEY 和 GET_REMOTEID_SESSION_KEY 功能相同，
      都是生成一个新的随机会话密钥并返回给请求方。
      会话密钥用于后续通信中的加密或认证。
      ---------------------------------------------------------------
    */
    case SECURE_COMMAND_GET_SESSION_KEY:
    case SECURE_COMMAND_GET_REMOTEID_SESSION_KEY: {
        /* 生成新的随机会话密钥，写入成员变量 session_key */
        make_session_key(session_key);

        /* 将会话密钥的完整内容拷贝到应答数据区 */
        reply.data_length = sizeof(session_key);
        memcpy(reply.data, session_key, reply.data_length);

        reply.result = MAV_RESULT_ACCEPTED;
        break;
    }

    /*
      ---------------------------------------------------------------
      获取公钥操作：
      从设备存储中检索指定范围的 Ed25519 公钥。
      请求数据格式（2 字节）：
        data[0] = key_idx   —— 起始公钥索引
        data[1] = num_keys  —— 需要获取的公钥数量
      应答数据格式：
        data[0] = key_idx（起始索引）
        data[1..] = 连续排列的公钥数据（每个 PUBLIC_KEY_LEN 字节）
      ---------------------------------------------------------------
    */
    case SECURE_COMMAND_GET_PUBLIC_KEYS: {
        /* 请求数据必须恰好为 2 字节：[起始索引, 数量] */
        if (pkt.data_length != 2) {
            reply.result = MAV_RESULT_UNSUPPORTED;
            goto send_reply;
        }

        const uint8_t key_idx = pkt.data[0];   /* 起始公钥索引 */
        uint8_t num_keys = pkt.data[1];         /* 请求获取的公钥数量 */

        /*
          计算应答缓冲区最多能容纳多少个公钥：
          reply.data[] 需要预留 1 字节存放起始索引（data[0]），
          剩余空间除以每个公钥的长度，即为最大可返回的公钥数。
        */
        const uint8_t max_fetch = (sizeof(reply.data)-1) / PUBLIC_KEY_LEN;

        /*
          边界检查：
          - key_idx 不能超出公钥存储上限（MAX_PUBLIC_KEYS）
          - 请求数量不能超过应答缓冲区容量（max_fetch）
          - 起始索引 + 数量不能越界
          - 设备上必须至少存有公钥（no_public_keys() 返回 true 表示无公钥）
        */
        if (key_idx >= MAX_PUBLIC_KEYS ||
            num_keys > max_fetch ||
            key_idx+num_keys > MAX_PUBLIC_KEYS ||
            g.no_public_keys()) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        /* 逐个读取公钥，依次写入应答缓冲区（偏移 1 字节留给索引） */
        for (uint8_t i=0;i<num_keys;i++) {
            g.get_public_key(i+key_idx, &reply.data[1+i*PUBLIC_KEY_LEN]);
        }

        /* 应答数据总长度 = 1 字节索引 + num_keys 个公钥的数据 */
        reply.data_length = 1+num_keys*PUBLIC_KEY_LEN;

        /* 应答的第一个字节存放起始公钥索引，供接收方定位 */
        reply.data[0] = key_idx;

        reply.result = MAV_RESULT_ACCEPTED;
        break;
    }

    /*
      ---------------------------------------------------------------
      设置（写入）公钥操作：
      将一个或多个 Ed25519 公钥写入设备存储的指定位置。
      请求数据格式：
        data[0] = key_idx —— 起始写入索引
        data[1..] = 连续排列的公钥数据（每个 PUBLIC_KEY_LEN 字节）
      公钥数量由 (data_length - 1) / PUBLIC_KEY_LEN 推算。
      ---------------------------------------------------------------
    */
    case SECURE_COMMAND_SET_PUBLIC_KEYS: {
        /*
          数据长度至少为 1 字节索引 + 1 个完整公钥，
          否则数据不完整，无法执行写入。
        */
        if (pkt.data_length < PUBLIC_KEY_LEN+1) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        const uint8_t key_idx = pkt.data[0];   /* 起始写入索引 */

        /* 根据有效载荷长度计算包含的公钥数量（整除取整，忽略不足一个公钥的尾部数据） */
        const uint8_t num_keys = (pkt.data_length-1) / PUBLIC_KEY_LEN;

        /* 计算结果为 0 表示数据不足一个完整公钥 */
        if (num_keys == 0) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        /*
          索引边界检查：
          - 起始索引不能超出存储上限
          - 起始索引 + 公钥数量不能越界
        */
        if (key_idx >= MAX_PUBLIC_KEYS ||
            key_idx+num_keys > MAX_PUBLIC_KEYS) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        /* 逐个写入公钥；任一写入失败则标记 failed */
        bool failed = false;
        for (uint8_t i=0; i<num_keys; i++) {
            failed |= !g.set_public_key(key_idx+i, &pkt.data[1+i*PUBLIC_KEY_LEN]);
        }

        /* 只要有任一公钥写入失败，整体返回 FAILED */
        reply.result = failed? MAV_RESULT_FAILED : MAV_RESULT_ACCEPTED;
        break;
    }

    /*
      ---------------------------------------------------------------
      删除公钥操作：
      按索引删除设备存储中的一个或多个公钥。
      请求数据格式（2 字节）：
        data[0] = key_idx   —— 起始删除索引
        data[1] = num_keys  —— 需要删除的公钥数量
      ---------------------------------------------------------------
    */
    case SECURE_COMMAND_REMOVE_PUBLIC_KEYS: {
        /* 请求数据必须恰好为 2 字节 */
        if (pkt.data_length != 2) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        const uint8_t key_idx = pkt.data[0];   /* 起始删除索引 */
        const uint8_t num_keys = pkt.data[1];   /* 要删除的公钥数量 */

        /* 数量为 0 无意义，直接返回失败 */
        if (num_keys == 0) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        /* 索引边界检查：确保删除范围不越界 */
        if (key_idx >= MAX_PUBLIC_KEYS ||
            key_idx+num_keys > MAX_PUBLIC_KEYS) {
            reply.result = MAV_RESULT_FAILED;
            goto send_reply;
        }

        /* 逐个删除指定范围内的公钥 */
        for (uint8_t i=0; i<num_keys; i++) {
            g.remove_public_key(key_idx+i);
        }

        reply.result = MAV_RESULT_ACCEPTED;
        break;
    }

    /*
      ---------------------------------------------------------------
      设置 RemoteID 配置操作：
      通过 NUL（'\0'）分隔的 "NAME=VALUE" 键值对批量设置设备参数。
      例如数据区内容可能为：
        "PARAM_A=123\0PARAM_B=hello\0"
      每个键值对以 '=' 分隔参数名和参数值。
      数据末尾会被强制添加 NUL 终止符以确保字符串安全。
      ---------------------------------------------------------------
    */
    case SECURE_COMMAND_SET_REMOTEID_CONFIG: {
        int16_t data_len = pkt.data_length;

        /*
          将有效载荷拷贝到局部缓冲区，并在末尾追加 NUL 终止符，
          确保后续的字符串操作（strlen / strchr）不会越界。
        */
        char data[pkt.data_length+1];
        memcpy(data, pkt.data, pkt.data_length);
        data[pkt.data_length] = 0;

        /*
          命令缓冲区的格式是以 NUL 字符分隔的多组 "NAME=VALUE" 键值对。
          循环逐条解析并应用每个配置项。
        */
        reply.result = MAV_RESULT_ACCEPTED;
        char *command = (char *)data;

        while (data_len > 0) {
            /* 获取当前键值对字符串的长度（不含 NUL 终止符） */
            uint8_t cmdlen = strlen(command);

            /* 查找 '=' 分隔符，将其替换为 NUL 以分离参数名和参数值 */
            char *eq = strchr(command, '=');
            if (eq != nullptr) {
                *eq = 0;  /* 将 '=' 替换为 NUL，使 command 指向参数名 */

                /* 调用参数管理器的 set_by_name_string 设置参数值（eq+1 指向值字符串） */
                if (!g.set_by_name_string(command, eq+1)) {
                    /* 设置失败，输出调试信息并标记整体结果为失败 */
                    mav_printf(MAV_SEVERITY_INFO, "set %s failed", command);
                    reply.result = MAV_RESULT_FAILED;
                } else {
                    /* 设置成功，输出确认信息 */
                    mav_printf(MAV_SEVERITY_INFO, "set %s OK", command);
                }
            }

            /* 移动指针到下一个键值对（跳过当前字符串及其 NUL 终止符） */
            command += cmdlen+1;
            data_len -= cmdlen+1;
        }
        break;
    }
    }

/*
  统一的应答发送标签：
  无论操作成功、失败或被拒绝，最终都通过此处发送应答消息。
  各分支可通过 goto send_reply 在提前退出时跳转到此处。
  正常执行完 switch-case 的分支也会顺序执行到这里。
*/
send_reply:
    /* 通过 MAVLink 通道发送安全命令应答结构体 */
    mavlink_msg_secure_command_reply_send_struct(chan, &reply);
}
