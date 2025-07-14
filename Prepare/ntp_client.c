/**********************************************************************
 *   Copyright: (C)2024 LingYun IoT System Studio
 *      Author: LiJiahui<2199250859@qq.com>
 *
 * Description: Temperature sensor DS18B20 driver on ISKBoard
 *
 *   ChangeLog:
 *        Version    Date       Author            Description
 *        V1.0.0  2025.07.5    LiJiahui      Release initial version
 *
 ***********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "ntp_client.h"
#include "usart.h"
#include "cmsis_os.h"
#include "main.h"


//static uint8_t byte;
uint8_t uart_rx_buffer[128];
static volatile uint16_t rx_index = 0;
static volatile bool data_ready = false;
// 用于接收来自ESP8266串口数据的缓冲区
static char rx_buffer[512];
/* 标志位，用于异步通信中表示是否已收到期望的响应
 * volatile：告诉编译器不要优化这个变量的访问，它可能会在中断函数、DMA、其他线程、外设等异步场景中被修改，
 * 所以每次访问这个变量都必须从内存中读，而不是用寄存器缓存值
*/
static volatile bool response_received = false;
static wifi_status_t wifi_status = WIFI_DISCONNECTED;

static ntp_error_t esp8266_send_command(const char *cmd, const char *expected_response, uint32_t timeout_ms);
static ntp_error_t esp8266_connect_wifi(const char *ssid, const char *password);
static ntp_error_t esp8266_create_udp_connection(const char *server, uint16_t port);
static ntp_error_t esp8266_send_ntp_packet(void);
static ntp_error_t esp8266_receive_ntp_response(ntp_packet_t *packet);
static void ntp_packet_to_time(const ntp_packet_t *packet, rtc_time_t *time);
static uint64_t ntp_timestamp_to_unix(uint64_t ntp_timestamp);
static void unix_timestamp_to_rtc_time(uint64_t unix_timestamp, rtc_time_t *time);
//static uint64_t byte_swap_64(uint64_t value);
static ntp_error_t esp8266_wait_response(const char *expected_response, uint32_t timeout_ms);

void check_stack_usage(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    UBaseType_t stack_high_water_mark = uxTaskGetStackHighWaterMark(current_task);

    printf("Current task stack high water mark: %lu\n", stack_high_water_mark);

    if (stack_high_water_mark < 100) {
        printf("WARNING: Stack usage is high! Possible stack overflow.\n");
    }
}

//接收回调函数
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        // 将接收到的字节存入缓冲区
        if (rx_index < sizeof(rx_buffer) - 1)
        {
            rx_buffer[rx_index++] = uart_rx_buffer[0];
            rx_buffer[rx_index] = '\0'; // 确保字符串终止
        }

        // 检查是否接收到完整的AT命令响应
        if (strstr(rx_buffer, "\r\nOK\r\n") ||
            strstr(rx_buffer, "\r\nERROR\r\n") ||
            strstr(rx_buffer, "SEND OK"))
        {
            data_ready = true;
            response_received = true;
        }

        // 检查是否接收到IPD数据（NTP响应）
        if (strstr(rx_buffer, "+IPD,0,48:"))
        {
            // 检查是否已接收完整的NTP数据包
            char *ipd_pos = strstr(rx_buffer, "+IPD,0,48:");
            if (ipd_pos && (rx_index - (ipd_pos - rx_buffer + 10)) >= NTP_PACKET_SIZE)
            {
                data_ready = true;
                response_received = true;
            }
        }

        // 重新启动单字节接收中断
        HAL_UART_Receive_IT(&huart2, uart_rx_buffer, 1);
    }
}



/**
 * @brief 启动UART接收中断
 */
static void start_uart_receive_interrupt(void)
{
    rx_index = 0;
    data_ready = false;
    response_received = false;
    memset(rx_buffer, 0, sizeof(rx_buffer));

    // 启动单字节接收中断
    HAL_UART_Receive_IT(&huart2, uart_rx_buffer, 1);
}


/* 初始化NTP客户端 */
ntp_error_t	ntp_init(void)
{
	ntp_printf("Initializing NTP client...\r\n");

	/* 发送“AT”指令，检查ESP8266是否在线且响应正常 */
	//ntp_printf("开始发送\r\n");

	start_uart_receive_interrupt();

	if(esp8266_send_command("AT\r\n", ESP8266_RESPONSE_OK, 1000) != NTP_SUCCESS)
	{
		ntp_printf("ESP8266 not responding\r\n");
		return NTP_ERROR_WIFI_CONNECT;
	}
	//ntp_printf("结束发送\r\n");
	/* 设置ESP8266为Station模式（客户端模式），使其能连接到wifi服务器 */
	if(esp8266_send_command("AT+CWMODE=1\r\n", ESP8266_RESPONSE_OK, 2000) != NTP_SUCCESS)
	{
		ntp_printf("Failed to set WiFi mode\r\n");
		return NTP_ERROR_WIFI_CONNECT;
	}

	/* 启用多连接模式，允许多个TCP或UDP连接.注：如果开启多连接，发送数据需指明ID */
	if(esp8266_send_command("AT+CIPMUX=1\r\n", ESP8266_RESPONSE_OK, 2000) != NTP_SUCCESS)
	{
		ntp_printf("Failed to enable multiple connection\r\n");
		return NTP_ERROR_WIFI_CONNECT;
	}

	ntp_printf("NTP client initialized successfully\r\n");
	return NTP_SUCCESS;
}


/* 连接到指定的WIFI网络 */
ntp_error_t ntp_wifi_connect(const char *ssid, const char *password)
{
	ntp_error_t	result;

	if(!ssid || !password)
	{
		return NTP_ERROR_INVALID_PARAM;
	}

	ntp_printf("Connecting to WiFi :%s\r\n", ssid);

	//更新全局WiFi状态为“正在连接”
	wifi_status = WIFI_CONNECTING;

	//执行连接WiFi
	result = esp8266_connect_wifi(ssid, password);

	//根据连接结果更新全局WiFi状态
	if(result == NTP_SUCCESS)
	{
		wifi_status = WIFI_CONNECTED;
		ntp_printf("WIFI connected successfully\r\n");
	}
	else
	{
		wifi_status = WIFI_ERROR;
		ntp_printf("WIFI connection failed\r\n");
	}

	return result;
}


/* 获取当前的WiFi连接状态 */
wifi_status_t ntp_get_wifi_status(void)
{
	return wifi_status;
}


/* 从NTP服务器获取网络时间
 * 参数：time：存储获取到的时间,rtc_time_t:定义在isl1208.h
*/
ntp_error_t ntp_get_time(rtc_time_t *time)
{
	const char 		*ntp_servers[] = {NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY, NTP_SERVER_TERTIARY};
	ntp_packet_t	ntp_packet;
	ntp_error_t		result = NTP_ERROR_RECEIVE_TIMEOUT; /* 默认结果为超时 */
	int				i;

	if(!time)
	{
		return NTP_ERROR_INVALID_PARAM;
	}

	//检查WiFi是否已连接
	if(wifi_status != WIFI_CONNECTED)
	{
		ntp_printf("wifi not connected\r\n");
		return NTP_ERROR_WIFI_CONNECT;
	}

	ntp_printf("Getting time from NTP server...\r\n");

	//循环尝试连接所有NTP服务器
	for(i=0; i<3; i++)
	{
		ntp_printf("Trying NTP server:%s\r\n", ntp_servers[i]);

		if(esp8266_create_udp_connection(ntp_servers[i], 123) != NTP_SUCCESS)
		{
			ntp_printf("Failed to create UDP connection to %s\r\n", ntp_servers[i]);
			continue;
		}

		//发送NTP请求包
		if(esp8266_send_ntp_packet() != NTP_SUCCESS)
		{
			ntp_printf("Failed to send NTP packet\r\n");
			//关闭当前UDP连接，然后尝试下一个服务器
			esp8266_send_command("AT+CIPCLOSE=0\r\n", ESP8266_RESPONSE_OK, 1000);
			continue;
		}

		//等待并接收NTP服务器的响应
		if(esp8266_receive_ntp_response(&ntp_packet) == NTP_SUCCESS)
		{
			ntp_printf("NTP response received from %s\r\n", ntp_servers[i]);
			result = NTP_SUCCESS;
			esp8266_send_command("AT+CIPCLOSE=0\r\n", ESP8266_RESPONSE_OK, 1000); //关闭连接
			break; //退出循环
		}

		//若当前服务器接受失败或超时，官邸当前UDP连接
		esp8266_send_command("AT+CIPCLOSE=0\r\n", ESP8266_RESPONSE_OK, 1000);
		//osDelay(1000);
		udelay(1000);
	}

	//所有服务器都尝试失败
	if(result != NTP_SUCCESS)
	{
		ntp_printf("Failed to get time from all NTP servers\r\n");
		return result;
	}

	//将收到的NTP数据包解析为日历时间
	ntp_packet_to_time(&ntp_packet, time);
	//打印获取到的时间
	ntp_printf("Time obtained:%04d-%02d-%02d %02d:%02d:%02d\r\n",
			time->tm_year, time->tm_mon, time->tm_mday,
			time->tm_hour, time->tm_min, time->tm_sec);

	return NTP_SUCCESS;
}


/* 同步RTC时间，将网络时间写入RTC芯片 */
ntp_error_t ntp_sync_rtc(void)
{
    extern i2c_bus_t i2c_bus[];
    rtc_time_t netword_time;

    ntp_error_t result = ntp_get_time(&netword_time);
    if (result != NTP_SUCCESS)
        return result;

    if (i2c_bus[ISL1208_I2CBUS].addr == 0x00)
    {
        printf("Initializing I2C bus %d with address 0x%02X\r\n", ISL1208_I2CBUS, ISL1208_CHIPADDR);
        if (i2c_init(ISL1208_I2CBUS, ISL1208_CHIPADDR) != 0)
        {
            printf("I2C initialization failed\r\n");
            return NTP_ERROR_RTC_SYNC;
        }
    }

    if (set_rtc_time(netword_time) != 0)
    {
        printf("Failed to sync RTC time\r\n");
        return NTP_ERROR_RTC_SYNC;
    }

    printf("Releasing I2C bus %d\r\n", ISL1208_I2CBUS);
    i2c_term(ISL1208_I2CBUS);

    printf("RTC synchronized successfully\r\n");
    return NTP_SUCCESS;
}



/* NTP FreeRTOS 任务函数 */
void ntp_task(void *pvParameters)
{
	//初始化NTP客户端
	if(ntp_init() != NTP_SUCCESS)
	{
		ntp_printf("NTP initialization failed\r\n");
		vTaskDelete(NULL); //初始化失败，删除此任务
		return ;
	}

	//连接WIFI
	if(ntp_wifi_connect(WIFI_SSID, WIFI_PASSWORD) != NTP_SUCCESS)
	{
		ntp_printf("WiFi connection failed\r\n");
		vTaskDelete(NULL); //初始化失败，删除此任务
		return ;
	}

	i2c_init(ISL1208_I2CBUS, ISL1208_CHIPADDR);
	//首次启动时，进行一次时间同步
	if(ntp_sync_rtc() == NTP_SUCCESS)
	{
		ntp_printf("Initial RTC sync completed\r\n");
	}


	//先不开启定时同步
}




/* ---静态函数--- */


/* 发送AT命令并等待响应
 * 参数：cmd：指令字符串； expected_response：期望的响应；timeout_us：超时时间（ms）
 */
static ntp_error_t esp8266_send_command(const char *cmd, const char *expected_response, uint32_t timeout_ms)
{
    // 如果命令为空字符串，只等待响应
    if (strlen(cmd) == 0)
    {
        return esp8266_wait_response(expected_response, timeout_ms);
    }

    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

     //发送AT命令
    if (HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), timeout_ms) != HAL_OK)
    {
        ntp_printf("HAL_UART_Transmit failed for command: %s\r\n", cmd);
        return NTP_ERROR_SEND_PACKET;
    }

    // 启动接收中断
    HAL_UART_Receive_IT(&huart2, uart_rx_buffer, 1);

    uint32_t start_time = HAL_GetTick();

    // 等待响应
    while ((HAL_GetTick() - start_time) < timeout_ms)
    {
        if (data_ready)
        {
            if (strstr(rx_buffer, expected_response) != NULL)
            {
                ntp_printf("Received expected response: %s\r\n", expected_response);
                return NTP_SUCCESS;
            }
            if (strstr(rx_buffer, "ERROR") != NULL)
            {
                ntp_printf("Error response received: %s\r\n", rx_buffer);
                return NTP_ERROR_WIFI_CONNECT;
            }
        }
        //osDelay(10);
        udelay(10);
    }

    ntp_printf("Response timeout for command: %s\r\n", cmd);
    return NTP_ERROR_RECEIVE_TIMEOUT;
}


/* 连接WiFi网络 */
static ntp_error_t esp8266_connect_wifi(const char *ssid, const char *password)
{
	char cmd[128];

	//断开已有连接
	esp8266_send_command("AT+CWQAP\r\n", ESP8266_RESPONSE_OK, 2000);

	//连接WiFi
	snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, password);

	return esp8266_send_command(cmd, ESP8266_RESPONSE_OK, 15000);
}



/* 使用AT指令创建UDP传输 */
static ntp_error_t esp8266_create_udp_connection(const char *server, uint16_t port)
{
	char cmd[128];

	//创建udp连接:"AT+CIPSTART=<link ID>,<type>,<remote IP>,<remote port>"
	//此处link ID固定为0，类型为UDP
	snprintf(cmd, sizeof(cmd), "AT+CIPSTART=0,\"UDP\",\"%s\",%d\r\n", server, port);

	return esp8266_send_command(cmd, ESP8266_RESPONSE_OK, 5000);
}


/* 使用AT指令发送NTP请求包
 * 注：此处发送请求包未填写t1，可能会存在几十毫秒到几百毫秒的误差，准确计算方法：current_time = ((T2 - T1) + (T3 - T4)) / 2 + T1
 * 但考虑到定时精度要求没有那么高，且做了定时同步，补偿RTC漂移，若后续需要精准定时，可加上t1，并修改计算方式
 */
static ntp_error_t esp8266_send_ntp_packet(void)
{
    uint8_t ntp_packet[NTP_PACKET_SIZE] = {0};
    char cmd[64];
    uint32_t start_time;

    // 构建NTP请求包
    ntp_packet[0] = 0x1B; // LI=0, VN=3, Mode=3

    // 发送AT+CIPSEND命令
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=0,%d\r\n", NTP_PACKET_SIZE);
    if(esp8266_send_command(cmd, ">", 2000) != NTP_SUCCESS)
    {
        return NTP_ERROR_SEND_PACKET;
    }


     //发送NTP数据包
    if(HAL_UART_Transmit(&huart2, ntp_packet, NTP_PACKET_SIZE, 5000) != HAL_OK)
    {
        ntp_printf("Failed to transmit NTP packet\r\n");
        return NTP_ERROR_SEND_PACKET;
    }

    ntp_printf("NTP packet transmitted successfully\r\n");

    // 等待"SEND OK"响应 - 修复这里的逻辑
    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

    start_time = HAL_GetTick();

    // 启动接收中断
    HAL_UART_Receive_IT(&huart2, uart_rx_buffer, 1);

    // 等待"SEND OK"响应
    while ((HAL_GetTick() - start_time) < 5000)
    {
        if (data_ready || response_received)
        {
            if (strstr(rx_buffer, "SEND OK") != NULL)
            {
                ntp_printf("SEND OK received\r\n");
                return NTP_SUCCESS;
            }
            if (strstr(rx_buffer, "ERROR") != NULL)
            {
                ntp_printf("SEND ERROR received: %s\r\n", rx_buffer);
                return NTP_ERROR_SEND_PACKET;
            }
        }
        //osDelay(10);
        udelay(10);
    }

    ntp_printf("SEND OK timeout\r\n");
    return NTP_ERROR_SEND_PACKET;
}

/* 新增：专门用于等待响应的函数 */
static ntp_error_t esp8266_wait_response(const char *expected_response, uint32_t timeout_ms)
{
    uint32_t start_time;

    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

    start_time = HAL_GetTick();

    // 启动接收中断
    HAL_UART_Receive_IT(&huart2, uart_rx_buffer, 1);

    // 等待响应
    while ((HAL_GetTick() - start_time) < timeout_ms)
    {
        if (data_ready || response_received)
        {
            if (strstr(rx_buffer, expected_response) != NULL)
            {
                ntp_printf("Received expected response: %s\r\n", expected_response);
                return NTP_SUCCESS;
            }
            if (strstr(rx_buffer, "ERROR") != NULL)
            {
                ntp_printf("Error response received: %s\r\n", rx_buffer);
                return NTP_ERROR_WIFI_CONNECT;
            }
        }
        //osDelay(10);
        udelay(10);
    }

    ntp_printf("Response timeout for: %s\r\n", expected_response);
    return NTP_ERROR_RECEIVE_TIMEOUT;
}


/* 等待并解析来自ESP8266的NTP响应数据，解析出的数据存储在packet结构体中 */
static ntp_error_t esp8266_receive_ntp_response(ntp_packet_t *packet)
{
    char *ipd_start, *data_start;
    uint32_t start_time;
    uint8_t ntp_data[NTP_PACKET_SIZE];
    int i, data_len;

    // 清空接收缓冲区
    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

    start_time = HAL_GetTick();

    // 启动单字节接收中断（与现有中断处理保持一致）
    HAL_UART_Receive_IT(&huart2, uart_rx_buffer, 1);

    // 等待接收完整的+IPD数据
    while((HAL_GetTick() - start_time) < NTP_TIMEOUT_MS)
    {
        // 检查是否收到+IPD标识
        ipd_start = strstr(rx_buffer, "+IPD,0,48:");
        if(ipd_start != NULL)
        {
            // 计算从缓冲区开始到+IPD数据部分的长度
            data_start = ipd_start + 10; // 跳过"+IPD,0,48:"

            // 检查是否已接收到完整的48字节NTP数据
            data_len = rx_index - (data_start - rx_buffer);
            if(data_len >= NTP_PACKET_SIZE)
            {
                // 提取NTP数据包
                for(i = 0; i < NTP_PACKET_SIZE; i++)
                {
                    ntp_data[i] = (uint8_t)data_start[i];
                }

                // 解析NTP数据包
                packet->li_vn_mode = ntp_data[0];
                packet->stratum = ntp_data[1];
                packet->poll = ntp_data[2];
                packet->precision = ntp_data[3];

                // 提取服务器发送时间戳（字节40-47）
                // 注意：需要按照大端字节序正确组装64位时间戳
                packet->trans_timestamp = 0;
                for(i = 0; i < 8; i++)
                {
                    packet->trans_timestamp = (packet->trans_timestamp << 8) | ntp_data[40 + i];
                }

                ntp_printf("NTP timestamp: 0x%016llX\r\n", packet->trans_timestamp);

                return NTP_SUCCESS;
            }
        }

        //osDelay(10);
        udelay(10);
    }

    // 超时处理
    ntp_printf("NTP receive timeout. Received data: %s\r\n", rx_buffer);
    return NTP_ERROR_RECEIVE_TIMEOUT;
}


/* 将收到的NTP数据包解析为日历时间 */
static void ntp_packet_to_time(const ntp_packet_t *packet, rtc_time_t *time)
{
	//获取服务器的发送时间戳
	uint64_t ntp_timestamp = packet->trans_timestamp;
	//将NTP时间戳转换为UNIX时间戳
	uint64_t unix_timestamp = ntp_timestamp_to_unix(ntp_timestamp);

	//根据设定的时区，对UNIX时间戳进行偏移
	unix_timestamp += (TIME_ZONE_OFFSET * 3600);
	//将最终的UNIX时间戳转换为日历格式（年/月/日/时/分/秒）
	unix_timestamp_to_rtc_time(unix_timestamp, time);
}


/* 将NTP时间戳（从1900，1，1开始算起的秒数）转换为UNIX时间戳（从1970年起算起的秒数） */
static uint64_t ntp_timestamp_to_unix(uint64_t ntp_timestamp)
{
	//NTP时间戳的高32位表示整数秒部分，小数部分精度太高，不关心
	uint32_t seconds = (uint32_t)(ntp_timestamp >> 32);
	//减去1900到1970的秒数差，得到UNIX时间戳
	return (uint64_t)(seconds - NTP_EPOCH_OFFSET);
}


/* 将UNIX时间戳转换为rtc_time_t日历结构 */
static void unix_timestamp_to_rtc_time(uint64_t unix_timestamp, rtc_time_t *time)
{
	uint32_t year = 1970;
	uint32_t days_in_year;
	uint32_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; //每个月的天数
	uint32_t month = 1;
	uint32_t total_days;

	//从总秒数计算出总天数和当天经过的秒数
	uint32_t days = unix_timestamp / 86400; //86400秒/天
	uint32_t seconds_in_day = unix_timestamp % 86400;

	//计算当前的时、分、秒
	time->tm_hour = seconds_in_day /3600;
	time->tm_min = (seconds_in_day % 3600) / 60;
	time->tm_sec = seconds_in_day % 60;

	/* ---计算年月日--- */
	//从1970年开始迭代
	//循环减去每年的天数，直到剩余天数不足一年
	while(days >= (days_in_year = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365))
	{
		days -=days_in_year;
		year++;
	}
	time->tm_year = year;

	//如果是闰年，二月为29天
	if(((year % 4 ==0 && year % 100 != 0) || year % 400 == 0))
	{
		days_in_month[1] = 29;
	}

	//循环减去每个月的天数，直到剩余天数不足一个月
	while(days >= days_in_month[month - 1])
	{
		days -= days_in_month[month - 1];
		month++;
	}
	time->tm_mon = month;
	time->tm_mday = days + 1; //剩余天数+1即为日期

	/* ---计算星期--- */
	//1970年1月1日时星期四。总天数+4后对7取模即可得到星期几（0=周日，1=周一。。。）
	total_days = (unix_timestamp / 86400) + 4;
	time->tm_wday = total_days % 7;
}

