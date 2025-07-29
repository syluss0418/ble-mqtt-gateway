/**********************************************************************
 *   Copyright: (C)2024 LingYun IoT System Studio
 *      Author: LiJiahui<2199250859@qq.com>
 *
 * Description: Temperature sensor DS18B20 driver on ISKBoard
 *
 *   ChangeLog:
 *        Version    Date       Author            Description
 *        V1.0.0  2025.7.5    LiJiahui      Release initial version
 *
 ***********************************************************************/

#ifndef __NET_CLIENT_H__
#define __NET_CLIENT_H__

#include <stdint.h>
#include <stdbool.h>
#include "miscdev.h"
#include "isl1208.h"
#include "i2c_bitbang.h"
#include "FreeRTOS.h"
#include "semphr.h"  // 用于信号量相关操作


//#define ISL1208_I2CBUS		I2CBUS0 /* ISL1208 on GPIO I2C bus0 */
#define WIFI_SSID		"ssid111"
#define WIFI_PASSWORD	"200400010"

#define NET_DEBUG_ENABLE 1
#ifdef NET_DEBUG_ENABLE
#define ntp_printf(...) printf("[NTP] " __VA_ARGS__)
#else
#define ntp_printf(...) do {} while(0)
#endif

// 接收缓冲区和当前写入位置（全局变量）
extern uint8_t uart_rx_buffer[128];
extern volatile uint16_t uart_rx_index;


//NET Server
/* 主NTP服务器地址，这是一个全球分布地服务器池，可以提供稳定可靠地时间服务 */
#define NTP_SERVER_PRIMARY		"pool.ntp.org"
/* 备用NTP服务器地址 */
#define NTP_SERVER_SECONDARY	"ntp.aliyun.com"
/* 第三NTP服务器地址 */
#define NTP_SERVER_TERTIARY		"cn.pool.ntp.org"

/* ---时区设置--- */
/* 时区偏移量，这里设置为8，代表中国时区（UTC+8） */
#define TIME_ZONE_OFFSET	8

/* NTP相关常量 */
/* NTP协议报文的长度，固定为48字节 */
#define NTP_PACKET_SIZE		48
/* NTP时间戳与UNIX时间戳之间的秒数差。NTP从1900.1.1开始计时，UNIX从1970.1.1开始计时，故NTP时间戳比UNIX时间戳多了70年的秒数
 * 故收到从NTP传来的时间后，需要减去该宏定义，UNIX时间戳为STM32/C语言/RTC事件处理方式使用
 */
#define NTP_EPOCH_OFFSET	2208988800UL
/* 等到NTP服务器响应的超时时间（毫秒） */
#define NTP_TIMEOUT_MS		10000
/* NTP请求失败后的重试次数 */
#define NTP_RETRY_COUNT		3


/* ---ESP8266  AT指令响应字符串--- */
#define ESP8266_RESPONSE_OK		"OK" /* 成功执行后的响应 */
#define ESP8266_RESPONSE_ERROR	"ERROR" /* 执行指令出错 */
#define ESP8266_RESPONSE_FAIT	"FAIL" /* 执行指令失败 */


/* ---函数返回值/错误码定义---
 * 在net_client.c里将使用该枚举类型定义函数，方便维护，可读性增强
 */
typedef enum{
	NTP_SUCCESS	= 0,				//函数执行成功
	NTP_ERROR_WIFI_CONNECT = -1,	//WIFI连接失败或ESP8266模块通信错误
	NTP_ERROR_UDP_CREATE = -2,		//创建UDP(socket)连接失败
	NTP_ERROR_SEND_PACKET = -3,		//发送NTP数据包失败
	NTP_ERROR_RECEIVE_TIMEOUT = -4, // 接收NTP响应超时
	NTP_ERROR_INVALID_RESPONSE = -5,// 收到无效的NTP响应
	NTP_ERROR_RTC_SYNC = -6,        // 同步RTC硬件时钟失败
	NTP_ERROR_INVALID_PARAM = -7    // 函数传入了无效的参数
} ntp_error_t;



/* ---WIFI连接状态--- */
typedef enum{
	WIFI_DISCONNECTED = 0,			//WIFI未连接
	WIFI_CONNECTED = 1,				//WIFI已连接
	WIFI_CONNECTING = 2,			//WIFI正在连接中
	WIFI_ERROR = -1					//WIFI连接发生错误
} wifi_status_t;



// --- NTP数据包结构体定义---
typedef struct {
    // 标志位：LI (闰秒指示器, 2 bits), VN (版本号, 3 bits), Mode (格式，3 bits)
    uint8_t li_vn_mode;
    // Stratum level: 系统时钟的层级（0-15），表示时钟源的精确度
    uint8_t stratum;
    // Poll interval: 最大轮询间隔，以2的N次方秒为单位
    uint8_t poll;
    // Precision: 系统时钟的精度，以2的N次方秒为单位
    uint8_t precision;
    // Root Delay: 到主时钟源的总往返延迟
    uint32_t root_delay;
    // Root Dispersion: 时钟相对于主时钟源的最大误差
    uint32_t root_dispersion;
    // Reference ID: 参考时钟源的标识
    uint32_t ref_id;
    // Reference Timestamp: 系统时钟最后一次被设定或校正的时间
    uint64_t ref_timestamp;
    // Origin Timestamp: 客户端发送NTP请求的时间戳
    uint64_t orig_timestamp;
    // Receive Timestamp: 服务器接收到客户端请求的时间戳
    uint64_t recv_timestamp;
    // Transmit Timestamp: 服务器回复客户端请求的时间戳（最需要的）
    uint64_t trans_timestamp;
} ntp_packet_t;


ntp_error_t ntp_init(void);
ntp_error_t ntp_get_time(rtc_time_t *time);
ntp_error_t ntp_sync_rtc(void);
wifi_status_t ntp_get_wifi_status(void);
ntp_error_t ntp_wifi_connect(const char *ssid, const char *password);
void ntp_task(void *pvParameters);
extern void check_stack_usage(void);



#endif
