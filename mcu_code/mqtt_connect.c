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

#include "mqtt_connect.h"


#include <time.h>

// 修复后的网络接收函数 - 增加静态缓冲区保存IPD数据
static uint8_t mqtt_data_buffer[256];  // 静态缓冲区保存MQTT数据
static int mqtt_data_length = 0;       // 当前缓冲区中的数据长度
static int mqtt_data_offset = 0;       // 当前读取偏移

extern char rx_buffer[512];
extern volatile uint16_t rx_index;
extern volatile bool data_ready;
extern volatile bool response_received;

struct NetworkContext
{
	UART_HandleTypeDef *huart;
};


// 全局变量
static MQTTContext_t g_mqtt_context; //	MQTT上下文结构体，维护MQTT连接状态和相关信息
static NetworkContext_t g_network_context;
static uint8_t g_mqtt_buffer[MQTT_BUFFER_SIZE];
static char g_payload_buffer[PAYLOAD_BUFFER_SIZE];
static iot_status_t g_iot_status = IOT_STATUS_DISCONNECTED;
static device_control_callback_t g_control_callback = NULL;


static int32_t network_send(NetworkContext_t* pNetworkContext, const void* pBuffer, size_t bytesToSend)
{
    // 在发送前检查连接状态
    if(esp8266_send_command("AT+CIPSTATUS\r\n", "STATUS:3", 2000) != NTP_SUCCESS)
    {
        printf("TCP connection not established\r\n");
        return -1;
    }

    // 打印发送的数据
    printf("MQTT send data (%zu bytes): ", bytesToSend);
    const uint8_t* data = (const uint8_t*)pBuffer;
    for (size_t i = 0; i < bytesToSend; i++) {
        printf("%02X ", data[i]);
    }
    printf("\r\n");

    char cmd[64];
    uint32_t start_time;

    // 清空接收缓冲区
    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

    // 发送AT+CIPSEND命令
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", (int)bytesToSend);

    if (HAL_UART_Transmit(&huart2, (uint8_t*)cmd, strlen(cmd), 3000) != HAL_OK) {
        printf("Failed to send CIPSEND command\r\n");
        return -1;
    }

    printf("CIPSEND command sent: %s", cmd);

    // 等待 ">" 提示符
    start_time = HAL_GetTick();
    bool prompt_received = false;

    while ((HAL_GetTick() - start_time) < 3000) {
        if (data_ready || response_received) {
            if (strstr(rx_buffer, ">") != NULL) {
                printf("Got '>' prompt\r\n");
                prompt_received = true;
                break;
            }
            if (strstr(rx_buffer, "ERROR") != NULL || strstr(rx_buffer, "FAIL") != NULL) {
                printf("CIPSEND ERROR: %s\r\n", rx_buffer);
                return -1;
            }
        }
        HAL_Delay(10);
    }

    if (!prompt_received) {
        printf("Timeout waiting for '>'\r\n");
        return -1;
    }

    // 清空缓冲区准备发送数据
    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

    // 发送MQTT数据包
    if (HAL_UART_Transmit(&huart2, (uint8_t*)pBuffer, bytesToSend, 5000) != HAL_OK) {
        printf("Failed to transmit MQTT packet\r\n");
        return -1;
    }

    printf("MQTT packet transmitted, waiting for SEND OK...\r\n");

    // 等待"SEND OK"响应
    start_time = HAL_GetTick();
    bool send_ok_received = false;

    while ((HAL_GetTick() - start_time) < 5000) {
        if (data_ready || response_received) {
            if (strstr(rx_buffer, "SEND OK") != NULL) {
                printf("SEND OK received\r\n");
                send_ok_received = true;
                break;
            }
            if (strstr(rx_buffer, "ERROR") != NULL || strstr(rx_buffer, "FAIL") != NULL) {
                printf("SEND ERROR: %s\r\n", rx_buffer);
                return -1;
            }
        }
        HAL_Delay(10);
    }

    if (!send_ok_received) {
        printf("SEND OK timeout, received: %s\r\n", rx_buffer);
        return -1;
    }

    // 清空缓冲区为接收数据做准备
    memset(rx_buffer, 0, sizeof(rx_buffer));
    rx_index = 0;
    data_ready = false;
    response_received = false;

    return (int32_t)bytesToSend;
}



static int32_t network_recv(NetworkContext_t* pNetworkContext, void* pBuffer, size_t bytesToRecv)
{
    printf("network_recv called, need to receive %zu bytes\r\n", bytesToRecv);
    printf("Current mqtt buffer: length=%d, offset=%d\r\n", mqtt_data_length, mqtt_data_offset);

    // 如果我们干净的静态缓冲区中还有数据，优先返回这些数据
    if (mqtt_data_offset < mqtt_data_length) {
        int available = mqtt_data_length - mqtt_data_offset;
        int copy_bytes = (bytesToRecv < available) ? bytesToRecv : available;

        memcpy(pBuffer, mqtt_data_buffer + mqtt_data_offset, copy_bytes);
        mqtt_data_offset += copy_bytes;

        printf("Returned %d bytes from static buffer\r\n", copy_bytes);
        const uint8_t* recv_data = (const uint8_t*)pBuffer;
        printf("Data: ");
        for (int i = 0; i < copy_bytes; i++) {
            printf("%02X ", recv_data[i]);
        }
        printf("\r\n");

        return copy_bytes;
    }

    // 静态缓冲区空了，重置它，并尝试从串口获取新数据
    mqtt_data_length = 0;
    mqtt_data_offset = 0;

    uint32_t start_time = HAL_GetTick();
    // 设置一个5秒的超时
    while ((HAL_GetTick() - start_time) < 5000)
    {
        // 等待串口中断通知已有数据
        if (data_ready || response_received)
        {
            // 检查是否收到了服务器关闭连接的指令
            if (strstr(rx_buffer, "CLOSED") != NULL) {
                printf("Connection closed by server.\r\n");
                // 清空缓冲区并返回0，通知core_mqtt连接已断开
                memset(rx_buffer, 0, sizeof(rx_buffer));
                rx_index = 0;
                return 0;
            }

            // 寻找IPD数据包头
            char *ipd_start = strstr(rx_buffer, "+IPD,");
            if (ipd_start != NULL)
            {
                printf("Found IPD data\r\n");
                char *colon = strchr(ipd_start, ':');
                if (colon != NULL)
                {
                    int expected_length = 0;
                    // 从IPD头中解析出数据长度
                    if (sscanf(ipd_start + 5, "%d", &expected_length) == 1 && expected_length > 0)
                    {
                        printf("IPD expected length: %d\r\n", expected_length);

                        // 计算IPD负载数据的真实起始地址
                        uint8_t *data_start = (uint8_t*)(colon + 1);
                        // 计算我们当前在缓冲区中已经收到了多少负载数据
                        int available_bytes = rx_index - (data_start - (uint8_t*)rx_buffer);

                        printf("Available bytes after colon: %d\r\n", available_bytes);

                        // 判断是否收到了一个完整的IPD包
                        if (available_bytes >= expected_length)
                        {
                            // 将纯净的IPD负载数据复制到我们的静态缓冲区中
                            memcpy(mqtt_data_buffer, data_start, expected_length);
                            mqtt_data_length = expected_length;
                            mqtt_data_offset = 0;

                            printf("Copied %d bytes to static MQTT buffer.\r\n", expected_length);

                            // **非常重要**: 从主接收缓冲区(rx_buffer)中移除已经处理过的数据
                            // 防止下一次循环时重复解析
                            int processed_len = (data_start - (uint8_t*)rx_buffer) + expected_length;
                            if (rx_index > processed_len) {
                                memmove(rx_buffer, rx_buffer + processed_len, rx_index - processed_len);
                                rx_index -= processed_len;
                                rx_buffer[rx_index] = '\0'; // 保证字符串结束
                            } else {
                                // 如果缓冲区恰好用完，则清空
                                memset(rx_buffer, 0, sizeof(rx_buffer));
                                rx_index = 0;
                            }
                            data_ready = false; // 重置标志位
                            response_received = false;

                            // 现在，从我们干净的静态缓冲区中拷贝数据给core_mqtt
                            int copy_bytes = (bytesToRecv < mqtt_data_length) ? bytesToRecv : mqtt_data_length;
                            memcpy(pBuffer, mqtt_data_buffer, copy_bytes);
                            mqtt_data_offset = copy_bytes;

                            printf("Returned %d bytes from new data\r\n", copy_bytes);
                            return copy_bytes;
                        }
                        else {
                             printf("Data incomplete, waiting for more... (need %d, have %d)\r\n",
                                   expected_length, available_bytes);
                        }
                    }
                }
            }

            // 如果到这里，说明收到了数据但不是一个完整的IPD包，重置标志位继续等待
            data_ready = false;
            response_received = false;
        }
        HAL_Delay(50); // 加个小延时防止CPU空转
    }

    printf("network_recv timeout, no complete data received\r\n");
    return 0; // 超时返回0
}



// 获取当前时间（毫秒）
static uint32_t get_time_ms(void)
{
    return (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC);
}

// MQTT事件回调函数
static void mqtt_event_callback(MQTTContext_t* pMqttContext,
                               MQTTPacketInfo_t* pPacketInfo,
                               MQTTDeserializedInfo_t* pDeserializedInfo)
{

    if ((pPacketInfo->type & 0xF0U) == MQTT_PACKET_TYPE_PUBLISH)
    {
        MQTTPublishInfo_t* pPublishInfo = pDeserializedInfo->pPublishInfo;

        // 将接收到的消息转换为字符串
        char received_message[PAYLOAD_BUFFER_SIZE] = {0};
        if (pPublishInfo->payloadLength < PAYLOAD_BUFFER_SIZE - 1)
        {
            memcpy(received_message, pPublishInfo->pPayload, pPublishInfo->payloadLength);
            received_message[pPublishInfo->payloadLength] = '\0';
        }

        printf("received down message: %s\n", received_message);

        // 解析命令并调用回调函数（传给main.c里的回调函数）
        if (g_control_callback != NULL)
        {
            // 简单的JSON解析 - 实际项目中建议使用JSON库
            if (strstr(received_message, "set_led"))
            {
                if (strstr(received_message, "\"on\""))
                {
                    g_control_callback("led", "on");
                } else if (strstr(received_message, "\"off\""))
                {
                    g_control_callback("led", "off");
                }
            }
        }
    }
}

// 设备控制回调函数
void device_control_handler(const char* command, const char* value)
{
    printf("receive command : %s = %s\n", command, value);

    if (strcmp(command, "led") == 0)
    {
        if (strcmp(value, "on") == 0)
        {
            printf("LED is turn on\n");
            // gpio_set_level(LED_GPIO, 1);
        }
        else if (strcmp(value, "off") == 0)
        {
            printf("LED is turn off\n");
            // gpio_set_level(LED_GPIO, 0);
        }
    }

    // 可以在这里添加其他设备控制逻辑
}


// 初始化华为云IoT
int huawei_iot_init(void)
{
    printf("Initing huawei iot...\r\n");

    g_network_context.huart = &huart2;  //将UART2句柄赋值给网络上下文，用于后续串口通信

    //配置MQTT库的网络传输接口，绑定发送和接收函数
    TransportInterface_t transport_interface = {
        .recv = network_recv,
        .send = network_send,
        .pNetworkContext = &g_network_context //网络上下文指针
    };


    //配置MQTT库的固定缓冲区，用于内部协议包的构建和解析
    MQTTFixedBuffer_t fixed_buffer = {
        .pBuffer = g_mqtt_buffer,
        .size = MQTT_BUFFER_SIZE
    };


    //传入网络接口、时间获取函数、事件回调函数和固定缓冲区
    //MQTT_Init：初始化一个MQTT上下文
    //传入的get_time_ms函数会被core_mqtt用来处理心跳、超时等时间相关的逻辑
    MQTTStatus_t mqtt_status = MQTT_Init(&g_mqtt_context,
                                        &transport_interface,
                                        get_time_ms,
                                        mqtt_event_callback,
                                       	&fixed_buffer);

    if (mqtt_status != MQTTSuccess)
    {
        printf("MQTT Init failure: %d\r\n", mqtt_status);
        g_iot_status = IOT_STATUS_ERROR;
        return -1;
    }

    printf("Huawei iot init successfully!\r\n");
    return 0;
}


// 连接WiFi并建立与华为云的TCP连接
int huawei_iot_connect_wifi(void)
{

	if(esp8266_send_command("AT+CWJAP=\"ssid111\",\"200400010\"\r\n", ESP8266_RESPONSE_OK, 1000) != NTP_SUCCESS)
	{
		printf("connect wifi failure\r\n");
		return -1;
	}


	if(esp8266_send_command("AT+CIPMODE=0\r\n", ESP8266_RESPONSE_OK, 1000) != NTP_SUCCESS)
	{
		printf("cipmode set failure\r\n");
		return -2;
	}

	if(esp8266_send_command("AT+CIPMUX=0\r\n", ESP8266_RESPONSE_OK, 1000) != NTP_SUCCESS)
	{
		printf("cipmode set failure\r\n");
		return -3;
	}


	if(esp8266_send_command("AT+CIPSTART=\"TCP\",\"5969442708.st1.iotda-device.cn-north-4.myhuaweicloud.com\",1883\r\n", ESP8266_RESPONSE_OK, 5000) != NTP_SUCCESS)
	{
	    printf("ssl connect failure\r\n");
	    return -4;
	}


	g_iot_status = IOT_STATUS_WIFI_CONNECTED;

	return 0;

}

// 连接MQTT Broker，在WiFi和TCP连接建立后，通过MQTT协议与Broker建立会话
int huawei_iot_connect_mqtt(void)
{
	//检查WiFi，注意，如果不连接WiFi将无法进行MQTT连接
    if (g_iot_status != IOT_STATUS_WIFI_CONNECTED)
    {
        printf("WiFi disconnected，can not connect to MQTT\r\n");
        return -1;
    }

    printf("Connecting Huawei cloud MQTT...\r\n");

    // 填充MQTT连接信息（连接参数隔一段时间会变化，连接之前去华为云重新获取参数确认传入的连接信息无误）
    MQTTConnectInfo_t connect_info = {0};
    connect_info.pClientIdentifier = MQTT_CLIENT_ID;
    connect_info.clientIdentifierLength = strlen(MQTT_CLIENT_ID);
    connect_info.pUserName = MQTT_USER_NAME;
    connect_info.userNameLength = strlen(MQTT_USER_NAME);
    connect_info.pPassword = MQTT_PASSWORD;
    connect_info.passwordLength = strlen(MQTT_PASSWORD);
    connect_info.keepAliveSeconds = 60;
    connect_info.cleanSession = true; //清理会话（每次连接都是新会话）

    bool session_present = false;
    //MQTT_Connect():用于建立与MQTT Broker 会话的核心功能，
    //封装了从发送CONNECT报文到接收CONNACK报文的整个流程，成功后更新MQTT上下文
    MQTTStatus_t status = MQTT_Connect(&g_mqtt_context,
                                      &connect_info,
                                      NULL, //遗嘱消息
                                      MQTT_CONNECT_TIMEOUT,
                                      &session_present);

    if (status != MQTTSuccess)
    {
        printf("MQTT connected failure: %d\r\n", status);
        g_iot_status = IOT_STATUS_ERROR;
        return -1;
    }

    printf("MQTT connected successfully\r\n");
    g_iot_status = IOT_STATUS_MQTT_CONNECTED;

    // 订阅下发命令主题
    MQTTSubscribeInfo_t subscribe_info = {0};
    subscribe_info.qos = MQTTQoS0;
    subscribe_info.pTopicFilter = TOPIC_COMMAND;
    subscribe_info.topicFilterLength = strlen(TOPIC_COMMAND);

    // 生成MQTT订阅消息的Packet ID（包标识符）
    // MQTT_Subscribe函数需要一个packetId参数，对于QoS0订阅，此值通常为0，
    // 但CoreMQTT库为了通用性，即使QoS0也可能要求非0的packetId。
    // 这里的逻辑是确保nextPacketId不会为0，并处理溢出。
    uint16_t packetId = g_mqtt_context.nextPacketId;
    // 递增 nextPacketId，并处理回绕（0xFFFF + 1 = 0x0000，但0是保留的，所以回到1）
    g_mqtt_context.nextPacketId = (uint16_t)(packetId + 1U);
    if (g_mqtt_context.nextPacketId == 0U)
    {
        g_mqtt_context.nextPacketId = 1U;
    }


    //MQTT_Subscribe():向MQTT Broker 发送订阅请求（SUBSCRIBE报文）,1表示要订阅一个主题
    status = MQTT_Subscribe(&g_mqtt_context, &subscribe_info, 1, packetId);
    if (status == MQTTSuccess)
    {
        printf("Subscribe topic successfully: %s\r\n", TOPIC_COMMAND);
    }
    else
    {
        printf("Subscribe topic failure: %d\r\n", status);
    }

    return 0;
}

// 上报温度数据
int huawei_iot_report_temperature(float temperature)
{
    if (g_iot_status != IOT_STATUS_MQTT_CONNECTED)
    {
        printf("MQTT disconnected，can't report data\r\n");
        return -1;
    }

    printf("Begin to report temperature\r\n");

    // 构建JSON数据
    snprintf(g_payload_buffer, PAYLOAD_BUFFER_SIZE,
             "{\"services\":[{\"service_id\":\"mqtt\",\"properties\":{\"temp\":%.2f}}]}",
             temperature);

    // 发布数据
    MQTTPublishInfo_t publish_info = {0};
    publish_info.qos = MQTTQoS0;
    publish_info.retain = false; //不保留消息
    publish_info.pTopicName = TOPIC_REPORT;
    publish_info.topicNameLength = strlen(TOPIC_REPORT);
    publish_info.pPayload = g_payload_buffer;
    publish_info.payloadLength = strlen(g_payload_buffer);

    //第三个参数0表示不适用Packet ID（QoS发布通常不需要Packet ID）
    //MQTT_Publish():构建并发送一个MQTT PUBLISH报文到已连接的Broker，对于QoS大于0的消息，它还会更新内部状态，以便后续跟踪消息的确认过程
    MQTTStatus_t status = MQTT_Publish(&g_mqtt_context, &publish_info, 0);
    if (status != MQTTSuccess)
    {
        printf("Temperature data report failure: %d\r\n", status);
        return -1;
    }

    printf("Temperature data report successfullly: %.2f\r\n", temperature);
    return 0;
}

// 上报自定义数据
int huawei_iot_report_custom_data(const char* json_data)
{
    if (g_iot_status != IOT_STATUS_MQTT_CONNECTED)
    {
        printf("MQTT disconnected，can not report data!\r\n");
        return -1;
    }

    if (strlen(json_data) >= PAYLOAD_BUFFER_SIZE)
    {
        printf("The data is too long to be sent.\r\n");
        return -1;
    }

    // 发布数据
    MQTTPublishInfo_t publish_info = {0};
    publish_info.qos = MQTTQoS0;
    publish_info.retain = false;
    publish_info.pTopicName = TOPIC_REPORT;
    publish_info.topicNameLength = strlen(TOPIC_REPORT);
    publish_info.pPayload = json_data;
    publish_info.payloadLength = strlen(json_data);

    MQTTStatus_t status = MQTT_Publish(&g_mqtt_context, &publish_info, 0);
    if (status != MQTTSuccess)
    {
        printf("custom data report failure: %d\r\n", status);
        return -1;
    }

    printf("custom data report successfully!: %s\r\n", json_data);
    return 0;
}


// 设置设备控制回调函数
void huawei_iot_set_control_callback(device_control_callback_t callback)
{
    g_control_callback = callback;
}

// 处理MQTT消息循环
//该函数需在主循环中周期性调用，以处理MQTT的接收、发送和心跳等操作，所以可以看到这里是永不返回的
void huawei_iot_process_loop(void)
{
    if (g_iot_status == IOT_STATUS_MQTT_CONNECTED)
    {
    	/* MQTT_ProcessLoop 将MQTT上下文作为参数传入，完成以下工作：
    	 * 1.发送心跳包；
    	 * 2.接收和解析入站消息（通过调用自己写的recv函数尝试从网络接收数据。如果接收到数据，它会智能解析这些数据，
    	 * 判断他们是Broker的心跳响应（PINGRESP）、下发的应用消息(PUBLISH)、订阅确认(SUBACK)等）；
    	 * 3.触发事件回调，如果接收到云端下发的控制命令，会调用应用程序回调函数去处理 */
        MQTTStatus_t status = MQTT_ProcessLoop(&g_mqtt_context);
        if (status != MQTTSuccess)
        {
            printf("MQTT loop process failure: %d\r\n", status);
            g_iot_status = IOT_STATUS_ERROR;
        }
    }
}

// 获取当前状态
iot_status_t huawei_iot_get_status(void)
{
    return g_iot_status;
}

// 断开连接
void huawei_iot_disconnect(void)
{
    if (g_iot_status == IOT_STATUS_MQTT_CONNECTED)
    {
        MQTT_Disconnect(&g_mqtt_context);
        printf("MQTT disconnected\n");
    }
    g_iot_status = IOT_STATUS_DISCONNECTED;
}

// 生成随机温度（用于测试）
float huawei_iot_get_random_temperature(void)
{
    return 25.0 + (rand() % 1000) / 100.0f; // 25.0 到 34.99 之间的随机温度
}
