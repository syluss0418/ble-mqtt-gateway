/**********************************************************************
 *   Copyright: (C)2024 LingYun IoT System Studio
 *      Author: LiJiahui<2199250859@qq.com>
 *
 * Description: The purpose of this code is to provide a simple C library,
 *              which providing software bit-bang of the I2C protocol on
 *              any GPIO pins for ISKBoard.
 *
 *   ChangeLog:
 *        Version    Date       Author            Description
 *        V1.0.0  2025.07.22    LiJiahui      Release initial version
 *
 ***********************************************************************/

#ifndef _MQTT_CONNECT_H__
#define _MQTT_CONNECT_H__

#include <string.h>
#include "miscdev.h"
#include <stdlib.h>

#include "core_mqtt.h"
#include "core_mqtt_state.h"
#include "core_mqtt_serializer.h"
#include "core_mqtt_config.h"
#include "transport_interface.h"
#include "ntp_client.h"
#include "usart.h"
#include "ble_send.h"


// 缓冲区大小
#define MQTT_BUFFER_SIZE        2048
#define PAYLOAD_BUFFER_SIZE     512

// 超时时间
//#define MQTT_CONNECT_TIMEOUT    10000
#define MQTT_PROCESS_TIMEOUT    1000

// Topic定义
#define TOPIC_REPORT     "$oc/devices/687ca704d582f200183d3b33_040210/sys/properties/report"
#define TOPIC_COMMAND    "$oc/devices/687ca704d582f200183d3b33_040210/sys/messages/down"


// 华为云IoT初始化状态
typedef enum {
    IOT_STATUS_DISCONNECTED = 0,
    IOT_STATUS_WIFI_CONNECTED,
    IOT_STATUS_MQTT_CONNECTED,
    IOT_STATUS_ERROR
} iot_status_t;

// 设备控制回调函数类型
typedef void (*device_control_callback_t)(const char* command, const char* value);

// 外部WiFi连接函数声明 - 你需要在外部实现
extern int esp8266_send_command(const char* command, int expected_response, int timeout);

// 公共接口函数
int huawei_iot_init(void);
int huawei_iot_connect_wifi(void);
int huawei_iot_connect_mqtt(void);
int huawei_iot_report_temperature(float temperature);
int huawei_iot_report_custom_data(const char* json_data);
void huawei_iot_set_control_callback(device_control_callback_t callback);
void huawei_iot_process_loop(void);
iot_status_t huawei_iot_get_status(void);
void huawei_iot_disconnect(void);
void device_control_handler(const char* command, const char* value);

// 工具函数
float huawei_iot_get_random_temperature(void);



#endif
