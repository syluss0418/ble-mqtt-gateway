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
#include <signal.h>

#include <dbus/dbus.h>
#include <mosquitto.h>
#include <json-c/json.h>

#include "ble_gateway.h"
#include "mqtt_gateway.h"


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



mqtt_device_config_t device_config = {
	.host = "5969442708.st1.iotda-device.cn-north-4.myhuaweicloud.com",
	.port = 1883,
	.client_id = "687ca704d582f200183d3b33_040210_0_0_2025072904",
	.username = "687ca704d582f200183d3b33_040210",
	.password = "327d73c2c112fd381f62dcb84728873d9a3f3ef7aa96d7ed8ba7de9befba24c7",
	.publish_topic = "$oc/devices/687ca704d582f200183d3b33_040210/sys/properties/report",
	.subscribe_topic = "$oc/devices/687ca704d582f200183d3b33_040210/sys/messages/down",
	.keepalive_interval = 60, //MQTT心跳间隔 60s
	.publish_interval_sec = 5 //数据发布间隔
};


void sigint_handler(int signum)
{
	printf("\nCaptured SIGINT signal (%d). Setting exit flag for graceful shutdown...\n", signum);
	keep_running = 0;
}



int main(int argc, char **argv)
{
	pthread_t	uplink_tid; //上行线程ID
	pthread_t	downlink_tid; //下行线程ID
	DBusError	err;
	int			rc;

	signal(SIGINT, sigint_handler);

	srand(time(NULL));

	printf("Main: Strating BLE-MQTT Gateway appliacation...\n");

	pthread_mutex_init(&dbus_mutex, NULL);
	pthread_mutex_init(&mqtt_mutex, NULL);

	//step 1:初始化D-Bus连接
	dbus_error_init(&err);
	//获取系统D-Bus总线连接
	global_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if(dbus_error_is_set(&err))
	{
		fprintf(stderr, "Main: Initial D-Bus connection error: %s\n", err.message);
		dbus_error_free(&err);
		return -1;
	}
	printf("Main: D-Bus system bus connected.\n");


	//step 2:初始化mosquitto 库和客户端实例
	mosquitto_lib_init();
	printf("Main: Mosquitto library initialized.\n");


	global_mosq = mosquitto_new(device_config.client_id, true, (void *)&device_config);
	if(!global_mosq)
	{
		fprintf(stderr, "Main: Failed to create Mosquitto instance: %s\n", strerror(errno));
		dbus_connection_unref(global_dbus_conn); //释放D-BUS连接
		mosquitto_lib_cleanup();
		return -2;
	}
	printf("Main: Mosquitto client instance created with Client ID: %s\n", device_config.client_id);

	
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
        fprintf(stderr, "Main: Failed to set username/password: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(global_mosq); // 销毁Mosquitto实例
        dbus_connection_unref(global_dbus_conn); // 释放D-Bus连接
        mosquitto_lib_cleanup(); // 清理Mosquitto库
        return 1; 
    }


    // step 3:创建上行线程 (BLE 通知 -> MQTT 发布)
    // 创建一个新线程，执行uplink_thread_func函数，不传递任何参数
    if (pthread_create(&uplink_tid, NULL, uplink_thread_func, NULL) != 0) 
	{
        fprintf(stderr, "Main: Failed to create uplink thread.\n");
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    printf("Main: Uplink thread created.\n");

    // step 4: 创建下行线程 (MQTT 订阅 -> BLE 写入)
    // 创建一个新线程，执行downlink_thread_func函数，不传递任何参数
    if (pthread_create(&downlink_tid, NULL, downlink_thread_func, NULL) != 0) 
	{
        fprintf(stderr, "Main: Failed to create downlink thread.\n");
        pthread_cancel(uplink_tid); // 如果下行线程创建失败，尝试取消上行线程
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    printf("Main: Downlink thread created.\n");

    printf("Main: Gateway application is running. Press Ctrl+C to exit.\n");

    // step 5: 等待线程完成 (主线程等待上行和下行线程执行完毕)
    pthread_join(uplink_tid, NULL);   // 等待上行线程结束
    pthread_join(downlink_tid, NULL); // 等待下行线程结束

    printf("Main: Received exit signal, cleaning up resources...\n");

    // step 6: 清理资源 (程序退出前释放所有已分配的资源)
    if (global_dbus_conn) 
	{
        printf("Main: Disconnecting from BLE device...\n");
        dbus_connection_unref(global_dbus_conn); // 释放D-Bus连接
    }
    if (global_mosq) 
	{
        mosquitto_disconnect(global_mosq); // 干净地断开MQTT连接
        mosquitto_destroy(global_mosq);    // 销毁Mosquitto客户端实例，释放内存
    }
    mosquitto_lib_cleanup(); // 清理Mosquitto库的内部资源

    printf("Main: Gateway application exited.\n");

    return 0; // 程序成功退出	
}

