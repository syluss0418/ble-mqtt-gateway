/*********************************************************************************
 *      Copyright:  (C) 2025 LingYun IoT System Studio
 *                  All rights reserved.
 *
 *       Filename:  ble_gateway.c
 *    Description:  This file 
 *                 
 *        Version:  1.0.0(2025年07月30日)
 *         Author:  Li Jiahui <2199250859@qq.com>
 *      ChangeLog:  1, Release initial version on "2025年07月30日 14时06分22秒"
 *                 
 ********************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <json-c/json.h>


#include "mqtt_gateway.h"
#include "ble_gateway.h"
#include "log.h"


extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running;
extern DBusConnection *global_dbus_conn;
extern mqtt_device_config_t device_config;

extern pthread_mutex_t dbus_mutex;
extern pthread_mutex_t mqtt_mutex;

static char* get_string_from_dbus_variant(DBusMessageIter *variant_iter);

//处理PropertiesChanged D-Bus 信号，提取并发布特性值
//当 BLE 特性（特别是启用了通知的特性）的值发生变化时，BlueZ 会发出 PropertiesChanged 信号
//此函数作为 D-Bus 消息处理的回调，解析该信号并处理其中包含的新的特性值
void handle_properties_changed(DBusMessage *msg)
{
	DBusMessageIter args;		  //主参数迭代器
	const char *iface;            // 接口名
	DBusMessageIter changed_props; // 改变的属性字典迭代器
	DBusMessageIter entry;        // 字典项迭代器 (键值对)
	const char *key;              // 属性键
	DBusMessageIter variant_iter; // 属性值（变体）迭代器)
	char *decoded_str = NULL;
	int hr, spo2;
	int	rc_pub;
	char json_payload_buffer[256];

	//初始化迭代器，指向消息msg 的第一个参数
	dbus_message_iter_init(msg, &args);

	//读取第一个参数：接口名（string）。通常是 "org.bluez.GattCharacteristic1"
	dbus_message_iter_get_basic(&args, &iface);
	dbus_message_iter_next(&args); //移动到下一个参数

	//递归进入第二个参数：changed_props 字典 (这是一个 a{sv} 类型，即字符串到变体的字典)
	dbus_message_iter_recurse(&args, &changed_props);

	//遍历changed_props字典中的每个键值对
	while(dbus_message_iter_get_arg_type(&changed_props) != DBUS_TYPE_INVALID)
	{
		dbus_message_iter_recurse(&changed_props, &entry); //进入字典的键值对迭代器

		dbus_message_iter_get_basic(&entry, &key); //读取键

		if(strcmp(key, "Value") == 0) //如果键是“Value”，说明特性值发生了变化

		{
			dbus_message_iter_next(&entry);
			dbus_message_iter_recurse(&entry, &variant_iter);

			log_info("---Notification received from %s---\n", dbus_message_get_path(msg));
			print_notify_value(&variant_iter); //打印通知的原始值

			//将接收道德通知数据通过MQTT发布到华为云
			if(mqtt_connected_flag)
			{
				decoded_str = get_string_from_dbus_variant(&variant_iter); //从D-BUS变体中获取解码后的字符串
				if(decoded_str)
				{
					hr = 0;
					spo2 = 0;
					
					if(sscanf(decoded_str, "HR:%d,SpO2:%d", &hr, &spo2) == 2)
					{
						log_info("Parsed HR: %d, Spo2: %d\n", hr, spo2);
						if((hr != 0 || spo2 != 0) && (hr > HR_THRESHOLD || spo2 < SPO2_THRESHOLD || hr < 60 ))
						{
							log_info("ALERT: HR(%d) > %d or Spo2 (%d) < %d. Sending warning command to BLE device.\n", hr, HR_THRESHOLD, spo2, SPO2_THRESHOLD);
							//发送Waring
							if(write_characteristic_value(global_dbus_conn, WRITABLE_CHARACTERISTIC_PATH, WARNING_CMD) < 0)
							{
								log_error("Failed to send WARING command to BLE device.\n");
							}
						}


						// 调用 build_huawei_property_json 函数构建符合华为云 IoTDA 格式的 JSON 字符串
                        build_huawei_property_json(json_payload_buffer, sizeof(json_payload_buffer), hr, spo2);

                        log_info("Publishing MQTT payload: %s\n", json_payload_buffer);

                        // 使用 mosquitto_publish 发布 MQTT 消息
                        // 参数：mosq_obj, mid(NULL表示自动生成), 主题, 负载长度, 负载内容, QoS等级(1), Retain标志(false)
                        pthread_mutex_lock(&mqtt_mutex);
							
						rc_pub = mosquitto_publish(global_mosq, NULL, device_config.publish_topic, strlen(json_payload_buffer), json_payload_buffer, 1, false);

						pthread_mutex_unlock(&mqtt_mutex);

                        if (rc_pub != MOSQ_ERR_SUCCESS) { // 检查发布结果
                            log_error("Failed to publish MQTT message, return code %d\n", rc_pub);
                        } 
						else 
						{
                            log_info("MQTT message published successfully.\n");
                        }		
					}
					else
					{
						log_error("Failed to parse HR and SpO2 from notification string: \"%s\"\n", decoded_str);
					}
					free(decoded_str);
				}
				else
				{
					log_error("Failed to get decoded string from D-Bus variant.\n");
				}
			}
			log_info("-----------------------------------\n");
		}
		dbus_message_iter_next(&changed_props); //移动到字典中的下一个键值对
	}
}


//向BLE 特征值写入数据
//通过 D-Bus 调用 BlueZ 的 GattCharacteristic1.WriteValue 方法，向指定的 BLE 特征值写入数据
int write_characteristic_value(DBusConnection *conn, const char *char_path, const char *cmd_str)
{
    DBusMessage *msg;               // D-Bus 消息指针
    DBusMessageIter args, array_iter, options_iter; // 各种参数迭代器
    DBusError err;                  // D-Bus 错误结构体

    dbus_error_init(&err); 

    // 创建方法调用消息：调用 org.bluez.GattCharacteristic1 接口的 WriteValue 方法
    msg = dbus_message_new_method_call(BLUEZ_BUS_NAME, char_path, "org.bluez.GattCharacteristic1", "WriteValue");
    if (!msg) // 检查消息是否成功创建
    {
        log_error("Failed to create D-BUS message for writevalue.\n");
        return -1;
    }

    // 初始化参数迭代器，用于构造 WriteValue() 方法的参数。
    // WriteValue 方法通常接受两个参数：
    // 1. 一个字节数组 (a{y})，即要写入的数据
    // 2. 一个选项字典 (a{sv})，例如写入类型 (Write With Response / Write Without Response)
    dbus_message_iter_init_append(msg, &args);

    // 添加第一个参数：要写入的字节数组 (ARRAY of BYTE 'y')
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "y", &array_iter);
    // 遍历输入字符串 cmd_str 的每个字符，并作为字节添加到 D-Bus 消息中
    for (size_t i = 0; i < strlen(cmd_str); ++i)
    {
        uint8_t b = (uint8_t)cmd_str[i];
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &b);
    }
    dbus_message_iter_close_container(&args, &array_iter); // 关闭字节数组容器

    // 添加第二个参数：选项字典 (ARRAY of DICT_ENTRY {string, variant})
    // 尽管目前是空的字典，也需要添加，因为 BlueZ API 需要这个参数。
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options_iter);
    dbus_message_iter_close_container(&args, &options_iter); // 关闭选项字典容器


	pthread_mutex_lock(&dbus_mutex);

    // 同步发送消息，等待回复。-1 表示无限等待超时。
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg); // 释放发送的消息

	pthread_mutex_unlock(&dbus_mutex);

    if (dbus_error_is_set(&err)) // 检查方法调用是否出错
    {
        log_error("WriteValue failed for %s: %s\n", char_path, err.message);
        dbus_error_free(&err); // 释放错误信息
        return -1; 
    }

    log_info("Successfully sent: \"%s\" to %s\n", cmd_str, char_path);
    if (reply) { // 如果收到回复消息
        dbus_message_unref(reply); // 释放回复消息对象
    }
    return 0;
}




//从D-BUS变体迭代器中解析出字符串
static char* get_string_from_dbus_variant(DBusMessageIter *variant_iter) 
{
	int	len = 0;
	int	i = 0;
	char	 *str = NULL;
	uint8_t byte_val;
    DBusMessageIter array_iter; // 用于遍历字节数组的迭代器
    dbus_message_iter_recurse(variant_iter, &array_iter); // 递归进入变体，获取内部的数组迭代器

    // 计算字符串长度（预先遍历一次以确定内存大小）
    DBusMessageIter temp_iter = array_iter; // 使用临时迭代器避免影响主迭代器
    while (dbus_message_iter_get_arg_type(&temp_iter) != DBUS_TYPE_INVALID) {
        len++;
        dbus_message_iter_next(&temp_iter);
    }

    // 分配内存给字符串 (+1 用于空终止符)
    str = (char*)malloc(len + 1);
    if (!str) { // 检查内存分配是否成功
        log_error("Memory allocation failed for string from D-Bus variant.\n");
        return NULL;
    }

    dbus_message_iter_recurse(variant_iter, &array_iter); // 重置迭代器到数组开头，准备复制数据
    while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID) {
        dbus_message_iter_get_basic(&array_iter, &byte_val); // 获取字节值
        str[i++] = (char)byte_val;                            // 复制字节到字符串
        dbus_message_iter_next(&array_iter);                 // 移动到下一个字节
    }
    str[len] = '\0'; // 空终止字符串

    return str; // 返回新创建的字符串
}




//打印接收到的通知值
void print_notify_value(DBusMessageIter *variant_iter)
{
	char	buffer[256];
	int		index = 0;

	DBusMessageIter array_iter; // 用于遍历字节数组的迭代器
	// 递归进入变体，获取内部的数组迭代器
	
	dbus_message_iter_recurse(variant_iter, &array_iter);

    // 转换为字符串打印
    dbus_message_iter_recurse(variant_iter, &array_iter); // 重新初始化迭代器，以便再次遍历
    while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID)
    {
        uint8_t byte_val;
        dbus_message_iter_get_basic(&array_iter, &byte_val); // 获取字节值
        // 判断是否是可打印的 ASCII 字符 (从空格到波浪线)
        if (byte_val >= 32 && byte_val <= 126)
        {
			if(index < sizeof(buffer) - 1)
				buffer[index++] = (char)byte_val;
        }
        else
        {
			if(index < sizeof(buffer) - 1)
				buffer[index++] = '.'; //非可打印字符用点代替
        }
        dbus_message_iter_next(&array_iter); // 移动到下一个字节
    }
	buffer[index] = '\0';

	log_info("Decoded string: \"%s\"\n", buffer);
}



//调用D-Bus 上的一个方法
int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method)
{
	DBusMessage *msg, *reply;
	DBusError	err;

	dbus_error_init(&err);

	//调用D-BUS上的一个方法，该函数是与Bluez进行D-BUS通信的基础，用于调用Bluez提供的各种方法
	msg = dbus_message_new_method_call(BLUEZ_BUS_NAME, path, interface, method);
	if(!msg)
	{
		log_error("Failed to create D-BUS message for method %s.\n", method);
		return -1;
	}

	pthread_mutex_lock(&dbus_mutex);

	//向D-BUS发送消息，并阻塞等待应答（此处无限等待超时）
	reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);

	//释放发送的消息
	dbus_message_unref(msg);

	pthread_mutex_unlock(&dbus_mutex);

	if(dbus_error_is_set(&err))
	{
		log_error("D-Bus call %s on %s failed: %s\n", method, path, err.message);
		dbus_error_free(&err);
		return -2;
	}
	if(reply)
	{
		dbus_message_unref(reply);
	}

	return 0;
}


/* ---上行线程函数--- */
//负责BLE连接管理，通知接受和数据上报到MQTT
//此线程会连接到BLE设备，启用特性通知，并持续监听来自BLE设备的通知信号
//当接收到通知时，它会解析数据并发布到MQTT
void *uplink_thread_func(void *arg)
{
	DBusMessage *msg = NULL;
	DBusError err;

	dbus_error_init(&err);

	log_info("Uplink Thread: Starting BLE operations...\n");


	//step 1:连接到BLE设备
	//通过D-Bus调用BlueZ的device1接口的Connect方法来连接指定MAC地址的BLE设备
	log_info("Uplink Thread: Connecting to Ble device %s...\n", BLE_DEVICE_MAC);
	if(call_method(global_dbus_conn, DEVICE_PATH, "org.bluez.Device1", "Connect") < 0)
	{
		log_error("Uplink Thread: Failed to connect to BLE device.\n");
		return NULL;
	}
	log_info("Uplink Thread: Successfully connected to BLE devices.\n");


	//step 2:启用特性通知
	//通过D-BUS 调用Bluez的GattCharacteristic1 接口的 StartNotify 方法，启用特定特征值的通知功能
	if(call_method(global_dbus_conn, NOTIFY_CHARACTERISTIC_PATH, "org.bluez.GattCharacteristic1", "StartNotify") < 0)
	{
		log_error("Uplink Thread: Failed to enable notification.\n");
		return NULL;
	}


	//step 3:添加D-BUS信号匹配规则
	//这告诉 D-Bus 守护进程，程序对 org.freedesktop.DBus.Properties 接口的 PropertiesChanged 信号感兴趣
	//这使得当 BLE 特性值（如通知特性）发生变化时，BlueZ 会向本程序发送相应的 D-Bus 信号
	dbus_bus_add_match(global_dbus_conn, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", &err);
	if(dbus_error_is_set(&err))
	{
		log_error("Uplink Thread: D-Bus match rule error: %s\n", err.message);
		dbus_error_free(&err);
		return NULL;
	}
	dbus_connection_flush(global_dbus_conn); //刷新D-BUS连接，确保规则立即生效
	log_debug("Uplink Thread: D-Bus signal match rule added for notification.\n");


	//主循环，持续监听BLE通知
	while(keep_running)
	{
		pthread_mutex_lock(&dbus_mutex);

		//处理D-Bus I/O (接收BLE 信号和回复)
		//100 ms 超时时间
		dbus_connection_read_write(global_dbus_conn, 100);

		//从D-BUS消息队列中弹出并处理消息
		msg = dbus_connection_pop_message(global_dbus_conn);
		pthread_mutex_unlock(&dbus_mutex);
		if(msg) //如果有消息
		{
			// 判断消息是否是 PropertiesChanged 信号，并且路径与关注的通知特性路径匹配
			// strstr(dbus_message_get_path(msg), NOTIFY_CHARACTERISTIC_PATH) 用于过滤出特定特征值的通知))
			if(dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged") && strstr(dbus_message_get_path(msg), NOTIFY_CHARACTERISTIC_PATH))
			{
				//处理通知，解析通知数据并通过MQTT发布
				handle_properties_changed(msg);
			}
			dbus_message_unref(msg); //释放处理过的D-BUS消息
		}

		usleep(100000);
	}

	log_info("Uplink Thread: Exiting...\n");
	return NULL;
}

