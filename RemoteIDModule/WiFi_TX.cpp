/*
  WiFi NAN 和 Beacon 广播驱动实现

  本模块实现两种 WiFi 广播方式发送 OpenDroneID 远程识别信息：
  1. NAN（邻居感知网络）模式：发送同步信标帧 + Action 帧
  2. Beacon（信标）模式：通过 Vendor IE（厂商信息元素）嵌入到标准 Beacon 帧中

  致谢：WiFi 底层调用参考了 https://github.com/sxjack/uav_electronic_ids
 */

#include "WiFi_TX.h"
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_system.h>
#include "parameters.h"

/*
  init() - WiFi 硬件初始化
  配置随机 MAC 地址（防止通过 MAC 追踪设备）、启动 SoftAP、设置带宽和发射功率。
  仅在首次调用时执行实际初始化，后续调用直接返回 true。
 */
bool WiFi_TX::init(void)
{
    if (initialised) {
        return true;
    }
    initialised = true;

    // 生成随机 MAC 地址，避免通过固定 MAC 地址追踪远程识别设备
    uint8_t mac_addr[6];
    generate_random_mac(mac_addr);

    mac_addr[0] |= 0x02;  // 设置 MAC 本地管理位（表示非全球唯一地址）
    mac_addr[0] &= 0xFE;  // 清除 MAC 多播位（确保为单播地址）

    // 将生成的随机 MAC 地址设置为 ESP32 的基础 MAC
    esp_base_mac_addr_set(mac_addr);

    if (g.webserver_enable == 0) {
        // Web 服务器未启用：创建可见 AP，不允许任何设备连接（maxConnection=0）
        WiFi.softAP(g.wifi_ssid, g.wifi_password, g.wifi_channel, false, 0);
    } else {
        // Web 服务器已启用：创建可见 AP，允许最多 4 个设备连接（用于 Web 配置）
        WiFi.softAP(g.wifi_ssid, g.wifi_password, g.wifi_channel, false, 4);
    }

    // 设置 WiFi 带宽为 HT20（20MHz），确保兼容性
    if (esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20) != ESP_OK) {
        return false;
    }

    // 保存随机 MAC 地址，后续用于构建 OpenDroneID 帧头
    memcpy(WiFi_mac_addr, mac_addr, 6);

    // 设置 WiFi 最大发射功率（从参数中读取 dBm 值并转换为 ESP32 功率等级）
    esp_wifi_set_max_tx_power(dBm_to_tx_power(g.wifi_power));

    return true;
}

/*
  transmit_nan() - 通过 WiFi NAN 模式广播远程识别数据
  NAN 广播由两帧组成：
  1. 同步信标帧（Sync Beacon）— 用于 NAN 时钟同步
  2. Action 帧 — 携带实际的 OpenDroneID 消息包数据
  两帧通过 esp_wifi_80211_tx() 直接发送原始 802.11 帧。
 */
bool WiFi_TX::transmit_nan(ODID_UAS_Data &UAS_data)
{
    init();

    uint8_t buffer[1024] {};

    // 第一步：构建并发送 NAN 同步信标帧
    int length;
    if ((length = odid_wifi_build_nan_sync_beacon_frame((char *)WiFi_mac_addr,
                  buffer, sizeof(buffer))) > 0) {
        if (esp_wifi_80211_tx(WIFI_IF_AP, buffer, length, true) != ESP_OK) {
            return false;
        }
    }

    // 第二步：构建并发送 NAN Action 帧（包含 OpenDroneID 消息包）
    if ((length = odid_wifi_build_message_pack_nan_action_frame(&UAS_data, (char *)WiFi_mac_addr,
                  ++send_counter_nan,
                  buffer, sizeof(buffer))) > 0) {
        if (esp_wifi_80211_tx(WIFI_IF_AP, buffer, length, true) != ESP_OK) {
            return false;
        }
    }

    return true;
}

/*
  transmit_beacon() - 通过 WiFi Beacon 模式广播远程识别数据
  将 OpenDroneID 消息包作为 Vendor IE（厂商特定信息元素）嵌入到
  ESP32 SoftAP 自动发送的 Beacon 帧和 Probe Response 帧中。

  Vendor IE 使用 OUI = FA:0B:BC，OUI Type = 0x0D（OpenDroneID 标准定义）。
  同时设置到 Probe Response 中，可提高手机端的数据更新频率
  （手机主动扫描时会触发 Probe Response）。
 */
bool WiFi_TX::transmit_beacon(ODID_UAS_Data &UAS_data)
{
    init();

    uint8_t buffer[1024] {};

    // 构建 Beacon 帧格式的消息包（使用临时 SSID，仅提取载荷数据）
    int length;
    if ((length = odid_wifi_build_message_pack_beacon_frame(&UAS_data, (char *)WiFi_mac_addr,
                   "UAS_ID_OPEN", strlen("UAS_ID_OPEN"),
                  1000/g.wifi_beacon_rate, ++send_counter_beacon, buffer, sizeof(buffer))) > 0) {

        // 从构建的帧中提取 OpenDroneID 载荷，封装为 Vendor IE 格式
        uint8_t header_offset = 58;  // Beacon 帧头部固定长度（跳过 MAC 头 + 固定字段）
        vendor_ie_data_t IE_data;
        IE_data.element_id = WIFI_VENDOR_IE_ELEMENT_ID;  // IE 元素 ID = 0xDD（Vendor Specific）
        IE_data.vendor_oui[0] = 0xFA;      // OpenDroneID OUI 第 1 字节
        IE_data.vendor_oui[1] = 0x0B;      // OpenDroneID OUI 第 2 字节
        IE_data.vendor_oui[2] = 0xBC;      // OpenDroneID OUI 第 3 字节
        IE_data.vendor_oui_type = 0x0D;    // OUI 子类型（OpenDroneID 标识）
        IE_data.length = length - header_offset + 4; // 载荷长度（+4 是 esp_wifi_set_vendor_ie 的定义要求）
        memcpy(IE_data.payload, &buffer[header_offset], length - header_offset);

        // 更新 Beacon 帧中的 Vendor IE（先移除旧的，再添加新的）
        if (esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        if (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        // 同时更新 Probe Response 帧中的 Vendor IE，提高手机端接收更新频率
        if (esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        if (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        return true;
	}
	else {
		return false;
	}
}


/*
  dBm_to_tx_power() - 将 dBm 功率值转换为 ESP32 发射功率寄存器值
  ESP32 的发射功率以 0.25dBm 为步进，寄存器值 = (dBm + 1.125) × 4
  有效范围：2 dBm 到 20 dBm
 */
uint8_t WiFi_TX::dBm_to_tx_power(float dBm) const
{
    if (dBm < 2) {
        dBm = 2;       // 最小发射功率 2 dBm
    }
    if (dBm > 20) {
        dBm = 20;      // 最大发射功率 20 dBm
    }
    return uint8_t((dBm+1.125) * 4);
}
