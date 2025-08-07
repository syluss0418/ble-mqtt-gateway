/*********************************************************************************
 *      Copyright:  (C) 2025 LingYun IoT System Studio
 *                  All rights reserved.
 *
 *       Filename:  main.c
 *    Description:  This file 
 *                 
 *        Version:  1.0.0(2025年04月26日)
 *         Author:  Li Jiahui <2199250859@qq.com>
 *      ChangeLog:  1, Release initial version on "2025年07月30日 10时35分22秒"
 *                 
 ********************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <libgen.h>

#include <dbus/dbus.h>
#include <mosquitto.h>
#include <json-c/json.h>

#include "ble_gateway.h"
#include "mqtt_gateway.h"
#include "config_parser.h"
#include "log.h"

//D-Bus 连接对象
DBusConnection *global_dbus_conn = NULL;
//Mosquitto 客户端实例
struct mosquitto *global_mosq = NULL;
//MQTT 连接状态（0 未连接，1 已连接）
volatile int mqtt_connected_flag = 0;
//程序运行控制标志，接收到SIGNINT信号时，此标志设为0
volatile int keep_running = 1;

//互斥锁
pthread_mutex_t	dbus_mutex;
pthread_mutex_t	mqtt_mutex;


// 全局配置变量
// MQTT 配置
mqtt_device_config_t device_config;
// BLE 配置
char BLE_DEVICE_MAC[32];
char DEVICE_PATH[256];
char NOTIFY_CHARACTERISTIC_PATH[512];
char WRITABLE_CHARACTERISTIC_PATH[512];
int HR_THRESHOLD;
int SPO2_THRESHOLD;
char WARNING_CMD[128];


void sigint_handler(int signum)
{
	log_debug("\nCaptured SIGINT signal (%d). Setting exit flag for graceful shutdown...\n", signum);
	keep_running = 0;
}


void print_usage(char *progname)
{
	log_info("Usage: %s [OPTIONS]\n", progname);
	log_info("-c(--config):Specify the path to the configuration file.\n");
	log_info("-d(--daemon): Set program running on background.\n");
	log_info("-l(--log): Set program to verbose output mode (print logs to stdout).\n");
	log_info("-h(--help): Display this help information.\n");
	return ;
}

//释放由strdup分配的内存
void cleanup_config()
{
    free(device_config.host);
    free(device_config.client_id);
    free(device_config.username);
    free(device_config.password);
    free(device_config.publish_topic);
    free(device_config.subscribe_topic);	
}


int main(int argc, char **argv)
{
	pthread_t	uplink_tid; //上行线程ID
	pthread_t	downlink_tid; //下行线程ID
	DBusError	err;
	int			rc;
	char		*progname = NULL;
	int			daemon_run = 0; //默认非后台运行
	char		*config_file = NULL;
	char		*log_file = "test.log";
	int			log_size = 1024;
	int			log_level = LOG_LEVEL_DEBUG;
	int			ch;
	
	struct option opts[] = {
		{"config", required_argument, NULL, 'c'},
		{"daemon", no_argument, NULL, 'd'},
		{"log", no_argument, NULL, 'l'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};


	progname = basename(argv[0]);

	while((ch = getopt_long(argc, argv, "c:dlh", opts, NULL)) != -1)
	{
		switch(ch)
		{
			case 'c':
				config_file = optarg;
				break;
			case 'd':
				daemon_run = 1;
				break;
			case 'l':
				log_file = "console";
				log_level = LOG_LEVEL_DEBUG;
				break;
			case 'h':
				print_usage(progname);
				return EXIT_SUCCESS;
			default:
				print_usage(progname);
				return EXIT_SUCCESS;

		}
	}

	log_open(log_file, log_level, log_size, LOG_LOCK_DISABLE);

	if(!config_file)
	{
		log_error("Error: Configuration file is not specified. Use -c or --config option.\n");
		print_usage(progname);
		return EXIT_SUCCESS;
	}

	//解析配置文件
	if(parse_json_config(config_file) != 0)
	{
		log_error("Error: Failed to parse JSON configuration file.\n");
		return -1;
	}

	signal(SIGINT, sigint_handler);

	if(daemon_run)
	{
		daemon(0, 0);
	}


	log_info("Main: Strating BLE-MQTT Gateway appliacation...\n");

	pthread_mutex_init(&dbus_mutex, NULL);
	pthread_mutex_init(&mqtt_mutex, NULL);

	//step 1:初始化D-Bus连接
	dbus_error_init(&err);
	//获取系统D-Bus总线连接
	global_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if(dbus_error_is_set(&err))
	{
		log_error("Main: Initial D-Bus connection error: %s\n", err.message);
		dbus_error_free(&err);
		return -1;
	}
	log_info("Main: D-Bus system bus connected.\n");


	//step 2:初始化mosquitto 库和客户端实例
	mosquitto_lib_init();
	log_info("Main: Mosquitto library initialized.\n");


	global_mosq = mosquitto_new(device_config.client_id, true, (void *)&device_config);
	if(!global_mosq)
	{
		log_error("Main: Failed to create Mosquitto instance: %s\n", strerror(errno));
		dbus_connection_unref(global_dbus_conn); //释放D-BUS连接
		mosquitto_lib_cleanup();
		return -2;
	}
	log_info("Main: Mosquitto client instance created with Client ID: %s\n", device_config.client_id);

	
    // 设置所有 MQTT 回调函数
    mosquitto_connect_callback_set(global_mosq, on_connect_cb);       // 连接成功或失败时调用
    mosquitto_message_callback_set(global_mosq, on_message_cb);       // 收到订阅消息时调用
    mosquitto_publish_callback_set(global_mosq, on_publish_cb);       // 消息发布成功时调用
    mosquitto_subscribe_callback_set(global_mosq, on_subscribe_cb);   // 订阅成功时调用
    mosquitto_disconnect_callback_set(global_mosq, on_disconnect_cb); // 断开连接时调用

    // 设置用户名和密码，用于MQTT代理的认证
    rc = mosquitto_username_pw_set(global_mosq, device_config.username, device_config.password);
    if (rc != MOSQ_ERR_SUCCESS) 
	{
        log_error("Main: Failed to set username/password: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(global_mosq); // 销毁Mosquitto实例
        dbus_connection_unref(global_dbus_conn); // 释放D-Bus连接
        mosquitto_lib_cleanup(); // 清理Mosquitto库
        return 1; 
    }

	//使用TLS/SSL加密连接
	if(device_config.ca_cert)
	{
		rc = mosquitto_tls_set(global_mosq,
								device_config.ca_cert,
								NULL, //client_cert_path
								NULL, //client_key_path
								NULL, //psk
								NULL); //psk_identify
	
		if(rc != MOSQ_ERR_SUCCESS)
		{
			log_error("Main: Failed to set TLS options: %s\n", mosquitto_strerror(errno));
			mosquitto_destroy(global_mosq);
			mosquitto_lib_cleanup();
			return 1;
		}
	}


    // step 3:创建上行线程 (BLE 通知 -> MQTT 发布)
    // 创建一个新线程，执行uplink_thread_func函数，不传递任何参数
    if (pthread_create(&uplink_tid, NULL, uplink_thread_func, NULL) != 0) 
	{
        log_error("Main: Failed to create uplink thread.\n");
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    log_debug("Main: Uplink thread created.\n");

    // step 4: 创建下行线程 (MQTT 订阅 -> BLE 写入)
    // 创建一个新线程，执行downlink_thread_func函数，不传递任何参数
    if (pthread_create(&downlink_tid, NULL, downlink_thread_func, NULL) != 0) 
	{
        log_error("Main: Failed to create downlink thread.\n");
        pthread_cancel(uplink_tid); // 如果下行线程创建失败，尝试取消上行线程
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    log_debug("Main: Downlink thread created.\n");

    log_info("Main: Gateway application is running. Press Ctrl+C to exit.\n");

    // step 5: 等待线程完成 (主线程等待上行和下行线程执行完毕)
    pthread_join(uplink_tid, NULL);   // 等待上行线程结束
    pthread_join(downlink_tid, NULL); // 等待下行线程结束

    log_info("Main: Received exit signal, cleaning up resources...\n");

    // step 6: 清理资源 (程序退出前释放所有已分配的资源)
    if (global_dbus_conn) 
	{
		log_info("Main: Sending disconnect command to BLE device before exiting...\n");
		// 调用写特性值的函数来发送断开连接命令
		call_method(global_dbus_conn, DEVICE_PATH, "org.bluez.Device1", "Disconnect");        
		log_info("Main: Disconnecting from BLE device...\n");
        dbus_connection_unref(global_dbus_conn); // 释放D-Bus连接
    }
    if (global_mosq) 
	{
        mosquitto_disconnect(global_mosq); // 干净地断开MQTT连接
        mosquitto_destroy(global_mosq);    // 销毁Mosquitto客户端实例，释放内存
    }
    mosquitto_lib_cleanup(); // 清理Mosquitto库的内部资源

	cleanup_config(); //释放配置文件分配的内存

	log_info("Main: Gateway application exited.\n");

    return 0; // 程序成功退出	
}

