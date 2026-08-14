/*
  ROMFS（只读文件系统）实现

  将 Web 界面的静态文件（HTML/JS/CSS/图片）以 gzip 压缩格式
  嵌入到固件 Flash 中，运行时通过文件名查找并提供流式读取。

  主要功能：
  - find()：按文件名查找嵌入文件
  - exists()：检查文件是否存在
  - find_stream()：返回流式读取接口（供 WebServer 使用）
  - find_string()：解压 gzip 文件并返回字符串（用于加载公钥等文本数据）

  文件数据定义在自动生成的 romfs_files.h 中。
 */
#include <Arduino.h>
#include "romfs.h"
#include "romfs_files.h"
#include <string.h>
#include "tinf.h"

// 按文件名查找嵌入文件，返回文件描述符指针
const ROMFS::embedded_file *ROMFS::find(const char *fname)
{
    for (const auto &f : files) {
        if (strcmp(fname, f.filename) == 0) {
            Serial.printf("ROMFS Returning '%s' size=%u len=%u\n",
                          fname, f.size, strlen((const char *)f.contents));
            return &f;
        }
    }
    Serial.printf("ROMFS not found '%s'\n", fname);
    return nullptr;
}

// 检查指定文件是否存在于 ROMFS 中
bool ROMFS::exists(const char *fname)
{
    return find(fname) != nullptr;
}

// 返回文件的流式读取接口（动态分配，调用者需 delete）
ROMFS_Stream *ROMFS::find_stream(const char *fname)
{
    const auto *f = find(fname);
    if (!f) {
        return nullptr;
    }
    return new ROMFS_Stream(*f);
}

// ==================== ROMFS_Stream 流式读取实现 ====================

// 返回文件总大小（压缩后）
size_t ROMFS_Stream::size(void) const
{
    return f.size;
}

// 返回文件名
const char *ROMFS_Stream::name(void) const
{
    return f.filename;
}

// 返回剩余可读字节数
int ROMFS_Stream::available(void)
{
    return f.size - offset;
}

// 批量读取数据到缓冲区
size_t ROMFS_Stream::read(uint8_t* buf, size_t size)
{
    const auto avail = available();
    if (size > avail) {
        size = avail;
    }
    memcpy(buf, &f.contents[offset], size);
    offset += size;
    return size;
}

// 查看下一个字节但不移动读取位置
int ROMFS_Stream::peek(void)
{
    if (offset >= f.size) {
        return -1;
    }
    return f.contents[offset];
}

// 读取单个字节并移动读取位置
int ROMFS_Stream::read(void)
{
    if (offset >= f.size) {
        return -1;
    }
    return f.contents[offset++];
}

/*
  find_string() - 解压 gzip 文件并返回字符串
  用于加载嵌入的文本数据（如公钥文件）。
  gzip 文件末尾 4 字节为解压后数据长度（小端序）。
  使用 uzlib 库进行 gzip 解压，不验证 CRC（节省 Flash 空间）。
  返回动态分配的字符串（调用者需 free()），失败返回 nullptr。
 */
const char *ROMFS::find_string(const char *name)
{
    const auto *f = find(name);
    if (!f) {
        return nullptr;
    }

    // gzip 文件末尾 4 字节 = 解压后数据长度（小端序）
    const uint8_t *p = &f->contents[f->size-4];
    uint32_t decompressed_size = p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
    
    uint8_t *decompressed_data = (uint8_t *)malloc(decompressed_size + 1);
    if (!decompressed_data) {
        return nullptr;
    }

    // explicitly null terimnate the data
    decompressed_data[decompressed_size] = 0;

    TINF_DATA *d = (TINF_DATA *)malloc(sizeof(TINF_DATA));
    if (!d) {
        ::free(decompressed_data);
        return nullptr;
    }
    uzlib_uncompress_init(d, NULL, 0);

    d->source = f->contents;
    d->source_limit = f->contents + f->size - 4;

    // assume gzip format
    int res = uzlib_gzip_parse_header(d);
    if (res != TINF_OK) {
        ::free(decompressed_data);
        ::free(d);
        return nullptr;
    }

    d->dest = decompressed_data;
    d->destSize = decompressed_size;

    // we don't check CRC, as it just wastes flash space for constant
    // ROMFS data
    res = uzlib_uncompress(d);

    ::free(d);
    
    if (res != TINF_OK) {
        ::free(decompressed_data);
        return nullptr;
    }

    return (const char *)decompressed_data;
}
