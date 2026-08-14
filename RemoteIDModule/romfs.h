/*
  ROMFS（只读文件系统）接口声明

  将 Web 界面静态文件（HTML/JS/CSS/图片）以 gzip 压缩格式嵌入固件 Flash，
  运行时通过文件名查找并提供流式读取。

  ROMFS 类：静态接口，按文件名查找嵌入文件
  ROMFS_Stream 类：继承 Arduino Stream，提供流式读取（供 WebServer 使用）
*/
#pragma once

#include <stdint.h>

class ROMFS_Stream;

// 只读文件系统 - 提供嵌入式文件的查找和读取
class ROMFS {
public:
    static bool exists(const char *fname);                  // 检查文件是否存在
    static ROMFS_Stream *find_stream(const char *fname);    // 返回流式读取接口（调用者需 delete）
    static const char *find_string(const char *name);       // 解压 gzip 文件并返回字符串（调用者需 free）

    // 嵌入文件描述符
    struct embedded_file {
        const char *filename;       // 文件名（如 "/index.html"）
        uint32_t size;              // 压缩后数据大小（字节）
        const uint8_t *contents;    // 指向 Flash 中的 gzip 压缩数据
    };

private:
    static const struct embedded_file *find(const char *fname);
    static const struct embedded_file files[];   // 文件表（定义在自动生成的 romfs_files.h 中）
};

// ROMFS 流式读取器 - 继承 Arduino Stream 接口，供 WebServer 流式发送文件
class ROMFS_Stream : public Stream
{
public:
    ROMFS_Stream(const ROMFS::embedded_file &_f) :
        f(_f) {}

    // write 接口不支持（只读文件系统）
    size_t write(const uint8_t *buffer, size_t size) override { return 0; }
    size_t write(uint8_t data) override { return 0; }
    size_t size(void) const;        // 返回文件总大小
    const char *name(void) const;   // 返回文件名

    int available() override;       // 剩余可读字节数
    int read() override;            // 读取单字节
    int peek() override;            // 查看下一字节（不移动位置）
    void flush() override {}        // 无操作（只读）
    size_t readBytes(char *buffer, size_t length) override {
        return read((uint8_t*)buffer, length);
    }

private:
    size_t read(uint8_t* buf, size_t size);     // 批量读取
    const ROMFS::embedded_file &f;              // 关联的文件描述符
    uint32_t offset = 0;                        // 当前读取偏移
};

