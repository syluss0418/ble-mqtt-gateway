/*********************************************************************************
 *      Copyright:  (C) 2025 LingYun IoT System Studio
 *                  All rights reserved.
 *
 *       Filename:  mqtt_gateway.c
 *    Description:  This file 
 *                 
 *        Version:  1.0.0(2025年07月30日)
 *         Author:  Li Jiahui <2199250859@qq.com>
 *      ChangeLog:  1, Release initial version on "2025年07月30日 11时40分22秒"
 *                 
 ********************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <json-c/json.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include "mqtt_gateway.h"
#include "ble_gateway.h"
#include "log.h"


extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running;
extern mqtt_device_config_t device_config;

extern pthread_mutex_t mqtt_mutex;


void build_huawei_property_json(char *buffer, size_t size, int hr_value, int spo2_value)
{
	snprintf(buffer, size,
			"{\"services\":[{\"service_id\":\"mqtt\",\"properties\":{\"HR\":%d,\"Spo2\":%d}}]}",
			hr_value, spo2_value);
}


/* ----- Mosquitto 回调函数----- */

//MQTT连接回调函数
void on_connect_cb(struct mosquitto *mosq_obj, void *userdata, int result)
{
	int		i;
	int		subscribe_rc;
	char	hex_buffer[512];
	int		buffer_index = 0;

	log_debug("DEBUS: on_connect_cb triggered with result: %d (%s)\n", result, mosquitto_connack_string(result));

	mqtt_device_config_t *cfg = (mqtt_device_config_t *)userdata;
	if(result == 0) //if connect success
	{
		log_info("MQTT: Connected to broker successfully.\n");
		mqtt_connected_flag = 1;

		log_info("MQTT: Subscribing to topic: %s\n", cfg->subscribe_topic);


		log_debug("DEBUG: Subscribe to topic Hex: ");
		for(i = 0; cfg->subscribe_topic[i] != '\0' && buffer_index < sizeof(hex_buffer) - 3; i++)
		{
			buffer_index += snprintf(&hex_buffer[buffer_index], sizeof(hex_buffer) - buffer_index, "%02x ", (unsigned char)cfg->subscribe_topic[i]);
		}
		hex_buffer[buffer_index] = '\0';
		log_debug("%s\n", hex_buffer);

		//发送订阅请求到MQTT代理（Qos 1）
		subscribe_rc = mosquitto_subscribe(mosq_obj, NULL, cfg->subscribe_topic, 1);
		if(subscribe_rc != MOSQ_ERR_SUCCESS)
		{
			fprintf(stderr, "MQTT: Failed to initiate subscribe request: %s\n", mosquitto_strerror(subscribe_rc));
		}
		else
		{
			log_info("MQTT: Subscribe request send successfullt to broker.\n");
		}
	}
	else //if connect failure
	{
		fprintf(stderr, "MQTT: Connection failed: %s\n", mosquitto_connack_string(result));
		mqtt_connected_flag = 0;
	}
}



//MQTT消息接收回调函数（处理下行指令）
void on_message_cb(struct mosquitto *mosq_obj, void *userdata, const struct mosquitto_message *msg)
{
	char *ble_cmd_to_send = NULL; 
	char *request_id = NULL;
	char response_topic[256];
	char response_payload[128];
	int	 rc_pub;
	const char *ble_payload_to_send = NULL;
	size_t	ble_payload_len = 0;
	json_object *json_obj = NULL;
	json_object *paras_obj = NULL;
	json_object *report_obj = NULL;
	const char *report_value = NULL;


	log_info("\n--- Dwonlink message received ---\n");
	log_info("Topic: %s\n", msg->topic);
	log_info("Message: %.*s\n", msg->payloadlen, (char *)msg->payload);
	log_info("------------------------------------\n\n");

    // 检查是否是命令请求主题
    if (strstr(msg->topic, "/sys/commands/request_id=") != NULL)
    {
        // 提取 request_id
        char *request_id_start = strstr(msg->topic, "request_id=");
        if (request_id_start != NULL)
        {
            request_id = request_id_start + strlen("request_id=");
            log_debug("DEBUG: Received command with request_id: %s\n", request_id);

            // 1. 构建响应主题
            snprintf(response_topic, sizeof(response_topic),
                     "$oc/devices/%s/sys/commands/response/request_id=%s",
                     device_config.username, request_id);

            // 2. 构建响应负载（简单的成功响应）
            snprintf(response_payload, sizeof(response_payload),
                     "{\"result_code\":0}");

            // 3. 发布响应消息
            pthread_mutex_lock(&mqtt_mutex);
            rc_pub = mosquitto_publish(mosq_obj, NULL, response_topic, strlen(response_payload), response_payload, 1, false);
            pthread_mutex_unlock(&mqtt_mutex);

            if (rc_pub != MOSQ_ERR_SUCCESS)
            {
                fprintf(stderr, "MQTT: Failed to publish command response: %s\n", mosquitto_strerror(rc_pub));
            }
            else
            {
                log_info("MQTT: Published command response to topic: %s\n", response_topic);
            }
        }
    }


	/* 解析JSON （云端下发的命令） */
	json_obj = json_tokener_parse(msg->payload);
	if( !json_obj )
	{
		fprintf(stderr, "JSON parsing failed for message payload, Forwarding original payload.\n");
		//解析失败，发送原始MQTT负载给BLE
		ble_payload_to_send = (const char *)msg->payload;
		ble_payload_len = msg->payloadlen;
	}
	else
	{
		if(json_object_object_get_ex(json_obj, "paras", &paras_obj) && json_object_object_get_ex(paras_obj, "report", &report_obj))
		{
			report_value = json_object_get_string(report_obj);
			log_debug("JSON Parse: Found parse:report: %s. Using this for BLE command.\n", report_value);

			//解析成功
			ble_payload_to_send = report_value;
			ble_payload_len = strlen(report_value);
		}
		else
		{
			//解析成功，但没有找到report字段，发送原始负载
			fprintf(stderr, "JSON parsing successded, but 'paras.report' field not found. Forwarding original payload.\n");
			ble_payload_to_send = (const char *)msg->payload;
			ble_payload_len = msg->payloadlen;
		}

		json_object_put(json_obj); //释放JSON对象

	}	



	//如果D-Bus 系统总线连接成功,尝试将MQTT 负载转发给BLE设备
	if(global_dbus_conn && ble_payload_to_send)
	{
		ble_cmd_to_send = (char *)malloc(msg->payloadlen + 1);
		if(ble_cmd_to_send)
		{
			memcpy(ble_cmd_to_send, ble_payload_to_send, ble_payload_len);
			ble_cmd_to_send[ble_payload_len] = '\0';

			log_info("Forwarding MQTT payload to BLE \"%s\" to %s\n", ble_cmd_to_send, WRITABLE_CHARACTERISTIC_PATH);
			//向BLE特性写入数据
			if(write_characteristic_value(global_dbus_conn, WRITABLE_CHARACTERISTIC_PATH, ble_cmd_to_send) < 0)
			{
				fprintf(stderr, "Failed to send BLE command to microcontroller.\n");
			}
			free(ble_cmd_to_send);
		}
		else
		{
			fprintf(stderr, "Memory allocation failed to BLE command.\n");
		}
	}
	else
	{
		fprintf(stderr, "D-Bus connection not available for BLE write.\n");
	}
}



//MQTT消息发布成功回调函数
void on_publish_cb(struct mosquitto *mosq_obj, void *userdata, int mid)
{
	log_info("MQTT: Message published successfully, Message ID: %d\n", mid);
}



//MQTT订阅回调函数
void on_subscribe_cb(struct mosquitto *mosq_obj, void *userdata, int mid,int qos_count, const int *granted_qos)
{
	log_info("MQTT: Topic subscribe successfully, Message ID: %d\n", mid);
}



//MQTT断开连接回调函数
void on_disconnect_cb(struct mosquitto *mosq_obj, void *userdata, int result)
{
	log_info("MQTT: Disconnected from broker, return code: %d\n", result);
	mqtt_connected_flag = 0;
}




//下行线程：负责MQTT连接管理和下行消息处理
//该线程会持续尝试连接MQTT代理，并在连接成功后监听下行消息
//ps：它不负责向MQTT周期性发布数据，发布操作现在由BLE线程负责
void *downlink_thread_func(void *arg)
{
	int			rc;
	int			loop_status_initial;
	int			loop_attemps = 0;
	const int	MAX_LOOP_ATTEMPTS = 10; //最大尝试连接次数
	const int	LOOP_TIMEOUT_MS = 100; //每次循环的超时时间

	//检查MOsquitto 客户端实例是否已再main线程中初始化
	if(!global_mosq)
	{
		fprintf(stderr, "Downlink Thread: MQTT client not initialized in main thread.\n");
		return NULL;
	}

	log_info("---Downlink Thread: MQTT communication loop ---");
	while(keep_running)
	{
		//连接到MQTT Broker
		rc = mosquitto_connect(global_mosq, device_config.host, device_config.port, device_config.keepalive_interval);
		if(rc != MOSQ_ERR_SUCCESS)
		{
			fprintf(stderr, "Downlink Thread: Failed to connect to MQTT broker: %s. Retrying in 5 seconds...\n", mosquitto_strerror(rc));
			sleep(5);
			continue ;
		}


		//确保在进入主循环之前，有足够的机会让Mosquitto客户端完成连接的实际握手和初始化设置（订阅主题等）
		do
		{
			loop_status_initial = mosquitto_loop(global_mosq, LOOP_TIMEOUT_MS, 1);
			if(loop_status_initial != MOSQ_ERR_SUCCESS && loop_status_initial != MOSQ_ERR_NO_CONN)
			{
				fprintf(stderr, "DEBUS: Initial loop after connect encountered error: %s\n", mosquitto_strerror(loop_status_initial));
				break; //遇到错误就退出do-while循环
			}
			usleep(10000);
			loop_attemps++;
		} while(loop_attemps < MAX_LOOP_ATTEMPTS && mqtt_connected_flag == 0 && keep_running);	//循环条件：
																								//1、未达到最大尝试次数
																								//2、MQTT 尚未连接成功
																								//3、程序未被要求停止

		//如果多次循环仍未连接成功，返回外层循环，重新尝试完整的连接过程
		if(mqtt_connected_flag == 0 && keep_running)
		{
			fprintf(stderr, "DEBUS: Initial connection/subscription loop timed out or failed, attempting full reconnect...\n");
			mosquitto_disconnect(global_mosq); //断开当前可能存在的半连接
			sleep(1);
			continue ;
		}
		log_debug("DEBUG: Finished initiao loop after connect. Continuing main loop.\n");


		//循环处理MQTT网络事件和下行消息的持续接收
		//存在重连机制，如果连接丢失，退出while循环重连
		while(mqtt_connected_flag && keep_running)
		{
			rc = mosquitto_loop(global_mosq, 100, 1);
			if(rc != MOSQ_ERR_SUCCESS)
			{
				if(rc == MOSQ_ERR_NO_CONN) //如果错误是连接丢失
				{
					log_debug("Downlink Thread: Moquitto loop reports no connection, breaking to reconnect.\n");
					mqtt_connected_flag = 0;
				}
				else //其他类型的错误
				{
					fprintf(stderr, "Downlink Thread: Moquitto loop error: %s. Attempting to reconnect...\n", mosquitto_strerror(rc));
					mosquitto_disconnect(global_mosq); //强制断开以触发重连
				}
				sleep(1);
				break ; //退出尝试重连
			}

			usleep(10000);
		}	
	}

	log_info("Downlink Thread: Exiting...\n");
	return NULL;
}


