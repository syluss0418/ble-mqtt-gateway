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

// --- 全局变量定义 ---
DBusConnection *global_dbus_conn = NULL;
struct mosquitto *global_mosq = NULL;
volatile int mqtt_connected_flag = 0; // 由 on_connect_cb 和 on_disconnect_cb 维护
volatile int keep_running = 1;

// 华为云MQTT配置 (使用您 mqtt_connect.c 中能工作的参数)
// 这将作为 extern 声明在 mqtt_gateway.c 和 ble_gateway.c 中被引用
mqtt_device_config_t device_config = {
    .host = "5969442708.st1.iotda-device.cn-north-4.myhuaweicloud.com",
    .port = 1883, // 保持 1883 端口
    .client_id = "687ca704d582f200183d3b33_040210_0_0_2025072904", // 您 mqtt_connect.c 中能工作的 Client ID
    .username = "687ca704d582f200183d3b33_040210",
    .password = "327d73c2c112fd381f62dcb84728873d9a3f3ef7aa96d7ed8ba7de9befba24c7", // 您 mqtt_connect.c 中能工作的 Password
    .publish_topic = "$oc/devices/687ca704d582f200183d3b33_040210/sys/properties/report",
    .subscribe_topic = "$oc/devices/687ca704d582f200183d3b33_040210/sys/messages/down",
    .keepalive_interval = 60,
    .publish_interval_sec = 5
};

// --- 日志回调函数 (可选，用于 Mosquitto 内部日志) ---
void my_log_callback(struct mosquitto *mosq, void *userdta, int level, const char *str)
{
    printf("MOSQUITTO_LOG: %s\n", str);
}

// --- 信号处理函数 ---
void sigint_handler(int signum) {
    printf("\nCaptured SIGINT signal (%d). Setting exit flag for graceful shutdown...\n", signum);
    keep_running = 0;
}

// --- 主函数 ---
int main(int argc, char **argv) {
    pthread_t uplink_tid, downlink_tid;
    DBusError err;
    int rc;

    signal(SIGINT, sigint_handler);
    srand(time(NULL));

    printf("Main: Starting BLE-MQTT Gateway application...\n");

    // 1. 初始化 D-Bus 连接
    dbus_error_init(&err);
    global_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Main: Initial D-BUS connection error: %s\n", err.message);
        dbus_error_free(&err);
        return 1;
    }
    printf("Main: D-BUS system bus connected.\n");

    // 2. 初始化 Mosquitto 库和客户端实例
    mosquitto_lib_init();
    printf("Main: Mosquitto library initialized.\n");

    // 关键：将 device_config 作为 userdata 传递给 mosquitto_new
    global_mosq = mosquitto_new(device_config.client_id, true, (void*)&device_config);
    if (!global_mosq) {
        fprintf(stderr, "Main: Failed to create Mosquitto instance: %s\n", strerror(errno));
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    printf("Main: Mosquitto client instance created with Client ID: %s\n", device_config.client_id);

    // 设置所有 MQTT 回调函数
    mosquitto_connect_callback_set(global_mosq, on_connect_cb);
    mosquitto_message_callback_set(global_mosq, on_message_cb);
    mosquitto_publish_callback_set(global_mosq, on_publish_cb);
    mosquitto_subscribe_callback_set(global_mosq, on_subscribe_cb);
    mosquitto_disconnect_callback_set(global_mosq, on_disconnect_cb);
    mosquitto_log_callback_set(global_mosq, my_log_callback);

    // 设置用户名和密码
    rc = mosquitto_username_pw_set(global_mosq, device_config.username, device_config.password);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Main: Failed to set username/password: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }

    // 设置 MQTT 协议版本为 3.1.1
    int mqtt_protocol_version = MQTT_PROTOCOL_V311;
    rc = mosquitto_opts_set(global_mosq, MOSQ_OPT_PROTOCOL_VERSION, &mqtt_protocol_version);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Main: Failed to set MQTT protocol version: %s\n", mosquitto_strerror(rc));
    } else {
        printf("Main: MQTT protocol version set to 3.1.1.\n");
    }

    // 3. 创建上行线程 (BLE 通知 -> MQTT 发布)
    if (pthread_create(&uplink_tid, NULL, uplink_thread_func, NULL) != 0) {
        fprintf(stderr, "Main: Failed to create uplink thread.\n");
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    printf("Main: Uplink thread created.\n");

    // 4. 创建下行线程 (MQTT 订阅 -> BLE 写入)
    if (pthread_create(&downlink_tid, NULL, downlink_thread_func, NULL) != 0) {
        fprintf(stderr, "Main: Failed to create downlink thread.\n");
        pthread_cancel(uplink_tid); // 尝试取消上行线程
        mosquitto_destroy(global_mosq);
        dbus_connection_unref(global_dbus_conn);
        mosquitto_lib_cleanup();
        return 1;
    }
    printf("Main: Downlink thread created.\n");

    printf("Main: Gateway application is running. Press Ctrl+C to exit.\n");

    // 5. 等待线程完成 (对于 IoT 网关，通常是无限运行，直到收到退出信号)
    pthread_join(uplink_tid, NULL);
    pthread_join(downlink_tid, NULL);

    printf("Main: Received exit signal, cleaning up resources...\n");

    // 6. 清理资源
    if (global_dbus_conn) {
        printf("Main: Disconnecting from BLE device...\n");
        // call_method 的参数需要根据实际情况来，这里假设 DEVICE_PATH 和 Disconnect 方法可用
        // 为了确保安全，这里暂时注释掉，避免因 BLE 状态不正确导致崩溃
        // call_method(global_dbus_conn, DEVICE_PATH, "org.bluez.Device1", "Disconnect");
        dbus_connection_unref(global_dbus_conn);
    }
    if (global_mosq) {
        mosquitto_disconnect(global_mosq); // 干净断开 MQTT
        mosquitto_destroy(global_mosq);    // 释放 Mosquitto 实例
    }
    mosquitto_lib_cleanup(); // 清理 Mosquitto 库

    printf("Main: Gateway application exited.\n");

    return 0;
}
