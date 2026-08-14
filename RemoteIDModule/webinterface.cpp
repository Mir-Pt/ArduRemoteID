/*
  webinterface.cpp -- Web 管理界面实现

  本文件实现了基于 WiFi 的 Web 服务器，主要功能包括：
  1. 通过 ROMFS（只读文件系统）提供静态 Web 资源（HTML/JS/CSS/图片）
  2. 通过 AJAX 端点 /ajax/status.json 返回实时 RID 状态 JSON
  3. 通过 /update 端点支持 OTA（空中升级）固件更新

  Web 服务器由两个自定义 RequestHandler 处理请求：
  - ROMFS_Handler：从 Flash 中的只读文件系统提供经 gzip 压缩的静态文件
  - AJAX_Handler：返回当前远程识别模块的实时状态 JSON

  OTA 固件更新流程：
  - 客户端上传固件二进制文件到 /update 端点
  - 上传开始时缓存前 16 字节（lead_bytes），用于后续签名验证
  - 数据分块写入 Flash 的 OTA 分区
  - 上传结束时写入额外的 0xFF 填充字节（一个扇区+1字节），
    强制刷新写缓冲区，确保签名数据已落盘
  - 调用 CheckFirmware::check_OTA_next() 验证固件签名
  - 签名验证通过后提交更新并重启，否则拒绝更新
 */

#include "webinterface.h"

#include <WiFi.h>          // WiFi 核心功能
#include <WiFiClient.h>    // WiFi 客户端连接
#include <WebServer.h>     // HTTP Web 服务器
#include <WiFiAP.h>        // WiFi 接入点（AP）模式
#include <ESPmDNS.h>       // mDNS 服务发现
#include <Update.h>        // ESP32 OTA 固件更新 API
#include "parameters.h"    // 系统参数（含 WiFi SSID/密码等）
#include "romfs.h"         // ROMFS 只读文件系统（存储 Web 静态资源）
#include "check_firmware.h" // 固件签名验证模块
#include "status.h"        // RID 状态信息（生成状态 JSON）

// HTTP Web 服务器实例，监听 80 端口
static WebServer server(80);

/*
  ROMFS 静态文件请求处理器

  从 Flash 中的 ROMFS 只读文件系统提供静态 Web 资源。
  ROMFS 中的文件以 gzip 格式存储，发送时添加 Content-Encoding: gzip 头，
  浏览器会自动解压显示。

  文件在 ROMFS 中的路径前缀为 "web/"，例如：
  - 请求 "/" 或 "/index.html" -> ROMFS 中查找 "web/index.html"
  - 请求 "/style.css"         -> ROMFS 中查找 "web/style.css"
 */
class ROMFS_Handler : public RequestHandler
{
    /*
      canHandle() -- 判断当前请求是否可由本处理器处理

      将 URI 映射到 ROMFS 路径（"/" -> "/index.html"，再加 "web" 前缀），
      然后检查该文件是否存在于 ROMFS 中。
     */
    bool canHandle(HTTPMethod method, String uri) {
        // 根路径 "/" 重定向到首页 "/index.html"
        if (uri == "/") {
            uri = "/index.html";
        }
        // 添加 ROMFS 中的目录前缀 "web"
        uri = "web" + uri;
        // 检查文件是否存在于只读文件系统中
        if (ROMFS::exists(uri.c_str())) {
            return true;
        }
        return false;
    }

    /*
      handle() -- 处理 HTTP 请求，返回 ROMFS 中的静态文件

      根据文件扩展名确定 Content-Type，然后以 gzip 流的方式发送文件内容。
     */
    bool handle(WebServer& server, HTTPMethod requestMethod, String requestUri) {
        // 根路径重定向到首页
        if (requestUri == "/") {
            requestUri = "/index.html";
        }
        // 拼接 ROMFS 中的完整路径
        String uri = "web" + requestUri;
        Serial.printf("handle: '%s'\n", requestUri.c_str());

        // 根据文件扩展名确定 MIME 类型，默认为 "text/html"
        const char *content_type = "text/html";
        const struct {
            const char *extension;
            const char *content_type;
        } extensions[] = {
            { ".js", "text/javascript" },
            { ".jpg", "image/jpeg" },
            { ".png", "image/png" },
            { ".css", "text/css" },
        };
        // 遍历扩展名表，匹配当前文件的 Content-Type
        for (const auto &e : extensions) {
            if (uri.endsWith(e.extension)) {
                content_type = e.content_type;
                break;
            }
        }

        // 从 ROMFS 获取文件的流式读取对象
        auto *f = ROMFS::find_stream(uri.c_str());
        if (f != nullptr) {
            // ROMFS 中的文件已预先 gzip 压缩，设置编码头让浏览器自动解压
            server.sendHeader("Content-Encoding", "gzip");
            // 以流方式发送文件内容（避免一次性加载到内存）
            server.streamFile(*f, content_type);
            // 释放流对象
            delete f;
            return true;
        }
        return false;
    }

} ROMFS_Handler;

/*
  AJAX 状态查询请求处理器

  处理 /ajax/status.json 端点的 GET 请求。
  前端页面通过定时 AJAX 轮询此端点，获取远程识别模块的实时状态信息，
  包括 GPS 位置、飞行器 ID、信号状态等，以 JSON 格式返回。
 */
class AJAX_Handler : public RequestHandler
{
    /*
      canHandle() -- 仅处理 /ajax/status.json 请求
     */
    bool canHandle(HTTPMethod method, String uri) {
        return uri == "/ajax/status.json";
    }

    /*
      handle() -- 调用 status_json() 生成当前 RID 状态的 JSON 字符串并返回
     */
    bool handle(WebServer& server, HTTPMethod requestMethod, String requestUri) {
        if (requestUri != "/ajax/status.json") {
            return false;
        }
        // 返回 200 状态码，Content-Type 为 application/json，内容为实时状态 JSON
        server.send(200, "application/json", status_json());
        return true;
    }

} AJAX_Handler;

/*
  WebInterface::init() -- 初始化 Web 服务器

  完成以下工作：
  1. 打印 WiFi AP 信息（SSID 和密码）
  2. 注册 AJAX 和 ROMFS 两个请求处理器
  3. 注册 /update 端点，用于接收 OTA 固件上传
  4. 启动 HTTP 服务器

  OTA 固件上传处理分为两个回调：
  - 完成回调：上传结束后检查结果，成功则返回 200 并重启，失败则返回 500
  - 上传回调：逐块处理上传数据，包含签名验证逻辑
 */
void WebInterface::init(void)
{
    // 打印 WiFi 接入点的 SSID 和密码
    Serial.printf("WAP start %s %s\n", g.wifi_ssid, g.wifi_password);
    // 获取 AP 模式的 IP 地址（通常为 192.168.4.1）
    IPAddress myIP = WiFi.softAPIP();

    // 注册请求处理器（注意：AJAX 优先于 ROMFS，避免被静态文件路由覆盖）
    server.addHandler( &AJAX_Handler );
    server.addHandler( &ROMFS_Handler );

    /*
      注册 OTA 固件更新端点 /update（POST 方法）

      第一个 lambda：上传完成后的响应回调
      - 检查 Update 对象是否有错误
      - 成功时返回 "OK" 并重启 ESP32
      - 失败时返回 "FAIL" 并延时等待
     */
    server.on("/update", HTTP_POST, []() {
        if (Update.hasError()) {
			// 更新失败，返回 500 错误
			server.sendHeader("Connection", "close");
		    server.send(500, "text/plain","FAIL");
		    Serial.printf("Update Failed: Update function has errors\n");
		    // 延时 5 秒，让客户端有时间接收响应
		    delay(5000);
		} else {
			// 更新成功，返回 200 并准备重启
			server.sendHeader("Connection", "close");
			server.send(200, "text/plain","OK");
			Serial.printf("Update Success: \nRebooting...\n");
			// 延时 1 秒确保响应发送完毕，然后重启
			delay(1000);
			ESP.restart();
		}
    /*
      第二个 lambda：上传数据处理回调（每接收一块数据调用一次）

      处理三个阶段：
      - UPLOAD_FILE_START：初始化 OTA 更新会话
      - UPLOAD_FILE_WRITE：逐块写入固件数据，同时缓存前 16 字节
      - UPLOAD_FILE_END：写入填充字节、验证签名、提交或拒绝更新
     */
    }, [this]() {
        HTTPUpload& upload = server.upload();
        // 获取下一个可用的 OTA 分区（ESP32 支持双分区交替升级）
        static const esp_partition_t* partition_new_firmware = esp_ota_get_next_update_partition(NULL); //get OTA partion to which we will write new firmware file;

        // === 阶段1：上传开始 ===
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            // 重置 lead_bytes 缓存计数
            lead_len = 0;

            // 以最大可用空间开始 OTA 写入会话
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
                Update.printError(Serial);
            }

        // === 阶段2：接收数据块 ===
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            /*
              缓存固件的前 16 字节（lead_bytes）。
              这些字节包含固件头部信息，后续签名验证需要用到。
              因为 OTA 写入会修改分区内容，所以必须在写入前先保存。
             */
            if (lead_len < sizeof(lead_bytes)) {
                // 计算本次需要缓存的字节数
                uint32_t n = sizeof(lead_bytes)-lead_len;
                if (n > upload.currentSize) {
                    n = upload.currentSize;
                }
                // 从上传缓冲区拷贝到 lead_bytes
                memcpy(&lead_bytes[lead_len], upload.buf, n);
                lead_len += n;
            }
            // 将当前数据块写入 OTA 分区
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }

        // === 阶段3：上传结束，验证签名 ===
        } else if (upload.status == UPLOAD_FILE_END) {
            /*
              写入额外的 0xFF 填充字节（一个 Flash 扇区 + 1 字节）。
              这是为了强制 Update 库将内部写缓冲区刷新到 Flash，
              确保固件数据完整落盘，然后才能正确读取并验证签名。
              0xFF 是 Flash 的空白值，不会影响固件有效内容。
             */
            uint32_t extra = SPI_FLASH_SEC_SIZE+1;
            while (extra--) {
                uint8_t ff = 0xff;
                Update.write(&ff, 1);
            }

            // 验证新固件的签名是否合法
            if (!CheckFirmware::check_OTA_next(partition_new_firmware, lead_bytes, lead_len)) {
                // 签名验证失败，拒绝更新
                Serial.printf("Update Failed: firmware checks have errors\n");
                server.sendHeader("Connection", "close");
                server.send(500, "text/plain","FAIL");
                delay(5000);
            } else if (Update.end(true)) {
                // 签名验证通过且 OTA 写入成功提交
                Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
                server.sendHeader("Connection", "close");
                server.send(200, "text/plain","OK");
            } else {
                // OTA 最终提交失败
                Update.printError(Serial);
                Serial.printf("Update Failed: Update.end function has errors\n");
                server.sendHeader("Connection", "close");
                server.send(500, "text/plain","FAIL");
                delay(5000);
            }
        }
    });

    Serial.printf("WAP started\n");
    // 启动 HTTP 服务器，开始监听 80 端口
    server.begin();
}

/*
  WebInterface::update() -- Web 服务器周期更新

  采用延迟初始化（lazy init）模式：
  - 首次调用时执行 init() 完成服务器配置和启动
  - 后续每次调用处理一轮 HTTP 客户端请求

  此函数应在主循环 loop() 中被周期性调用，
  以保证 Web 服务器能及时响应客户端请求。
 */
void WebInterface::update()
{
    // 延迟初始化：仅在首次调用时启动 Web 服务器
    if (!initialised) {
        init();
        initialised = true;
    }
    // 处理当前待处理的 HTTP 客户端请求
    server.handleClient();
}
