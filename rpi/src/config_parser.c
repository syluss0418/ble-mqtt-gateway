#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <json-c/json.h>

#include "config_parser.h"
#include "mqtt_gateway.h"
#include "ble_gateway.h"


extern mqtt_device_config_t device_config;
extern char BLE_DEVICE_MAC[32];
extern char DEVICE_PATH[256];
extern char NOTIFY_CHARACTERISTIC_PATH[512];
extern char WRITABLE_CHARACTERISTIC_PATH[512];
extern int HR_THRESHOLD;
extern int SPO2_THRESHOLD;
extern char WARNING_CMD[128];


//JSON解析帮助函数：安全地获取指定键对应的字符串指针
static const char *get_json_string(json_object *obj, const char *key)
{
	json_object *field;
	
	//查找指定的键如果找到将值存在field中并返回true
	if(json_object_object_get_ex(obj, key, &field))
	{
		return json_object_get_string(field);
	}

	return NULL;
}


//JSON解析帮助函数：安全地从一个JSON对象中获取指定键的整数值
static int get_json_int(json_object *obj, const char *key)
{
	json_object *field;
	if(json_object_object_get_ex(obj, key, &field))
	{
		return json_object_get_int(field);
	}

	return 0;
}


/* 解析指定的JSON配置文件，并填充到全局配置变量中 */
int parse_json_config(const char *filename)
{
	//从文件中加载并解析JSON数据，返回一个根JSON对象
	json_object *root = json_object_from_file(filename);
	if(!root)
	{
		fprintf(stderr, "Error: Failed to parse JSON file '%s'.\n", filename);
		return -1;
	}

	//1.解析“mqtt_config”配置项
	json_object *mqtt_config;
	
	//尝试获取子对象
	if(json_object_object_get_ex(root, "mqtt_config", &mqtt_config))
	{
		//逐一解析子对象中的键值对，并填充到全局结构体中
		//strdup函数用于动态分配内存并复制字符串，因为JSON对象的生命周期可能比全局变量短
		/* 使用strdup:从json-c库中获取的字符串指针，所指向的内存是属于JSON对象的，当程序执行到最后，‘json_object_put(root)’会释放整个JSON对象，包括它内部的所有字符串。如果那时候还在使用这些字符串指针，它们就会变成悬空指针，指向已经被释放的内存区域，这会导致未定义的行为，通常是程序崩溃 */
		device_config.host = strdup(get_json_string(mqtt_config, "host"));
		device_config.port = get_json_int(mqtt_config, "port");
		device_config.client_id = strdup(get_json_string(mqtt_config, "client_id"));
		device_config.username = strdup(get_json_string(mqtt_config, "username"));
		device_config.password = strdup(get_json_string(mqtt_config, "password"));
		device_config.publish_topic = strdup(get_json_string(mqtt_config, "publish_topic"));
		device_config.subscribe_topic = strdup(get_json_string(mqtt_config, "subscribe_topic"));
		device_config.keepalive_interval = get_json_int(mqtt_config, "keepalive_interval");
		device_config.publish_interval_sec = get_json_int(mqtt_config, "publish_interval_sec");
		
		device_config.ca_cert = strdup(get_json_string(mqtt_config, "ca_cert"));
	}
	else //找不到mqtt_config，清理资源并返回
	{ 
		fprintf(stderr, "Error: 'mqtt_config' section not found in JSON.\n");
		json_object_put(root);
		return -1;
	}

	char ble_notify_suffix[256] = {0};
	char ble_write_suffix[256] = {0};

	//2.解析”ble_config“配置段
	json_object *ble_config;
	//获取子对象
	if(json_object_object_get_ex(root, "ble_config", &ble_config))
	{
		strncpy(BLE_DEVICE_MAC, get_json_string(ble_config, "device_mac"), sizeof(BLE_DEVICE_MAC) - 1);
		strncpy(ble_notify_suffix, get_json_string(ble_config, "notify_char_path_suffix"), sizeof(ble_notify_suffix) - 1);
		strncpy(ble_write_suffix, get_json_string(ble_config, "write_char_path_suffix"), sizeof(ble_write_suffix) - 1);
	}
	else
	{
		fprintf(stderr, "Error: 'ble_config' section not found in JSON.\n");
		json_object_put(root);
		return -2;
	}


	//3.解析”logic_thresholds"配置项
	json_object *logic_thresholds;
	//获取子对象
	if(json_object_object_get_ex(root, "logic_thresholds", &logic_thresholds))
	{
		HR_THRESHOLD = get_json_int(logic_thresholds, "hr_threshold");
		SPO2_THRESHOLD = get_json_int(logic_thresholds, "spo2_threshold");
		strncpy(WARNING_CMD, get_json_string(logic_thresholds, "warning_cmd"), sizeof(WARNING_CMD) - 1);
	}
	else
	{
		fprintf(stderr, "Error: 'logic_thresholds' section not found in JSON.\n");
		json_object_put(root);
		return -3;
	}


	//4.根据解析出的数据，构建完整的D-BUS路径
	if(strlen(BLE_DEVICE_MAC) > 0)
	{
		//构建设备路径
		snprintf(DEVICE_PATH, sizeof(DEVICE_PATH), "%s/dev_%s", ADAPTER_PATH, BLE_DEVICE_MAC);
		
		//如果通知特征后缀不为空，构建完整的通知特征路径
		if(strlen(ble_notify_suffix) > 0)
		{
			snprintf(NOTIFY_CHARACTERISTIC_PATH, sizeof(NOTIFY_CHARACTERISTIC_PATH), "%s/%s", DEVICE_PATH, ble_notify_suffix);
		}

		//如果可写特征后缀不为空，构建完整的通知特征路径
		if(strlen(ble_write_suffix) > 0)
		{
			snprintf(WRITABLE_CHARACTERISTIC_PATH, sizeof(WRITABLE_CHARACTERISTIC_PATH), "%s/%s", DEVICE_PATH, ble_write_suffix);
		}
	}

	//清理
	json_object_put(root);

	printf("JSON config loaded successfully.\n");
	printf("BLE Device Path: %s\n", DEVICE_PATH);
	printf("MQTT Client ID: %s\n", device_config.client_id);

	return 0;
}



