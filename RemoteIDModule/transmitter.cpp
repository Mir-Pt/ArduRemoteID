/*
  common functions for all transmitter backends
  广播发射器基类的通用函数实现
 */

#include "transmitter.h"

/*
  生成随机 MAC 地址
  用于 WiFi/BLE 广播时的源地址随机化，防止设备被长期追踪
 */
void Transmitter::generate_random_mac(uint8_t mac[6])
{
    for (uint8_t i=0; i<6; i++) {
        mac[i] = uint8_t(random(256));
    }
}
