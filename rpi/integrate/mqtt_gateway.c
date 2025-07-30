#include "mqtt_gateway.h"
#include "ble_gateway.h" // 包含 ble_gateway.h 来获取 write_characteristic_value 函数原型

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <json-c/json.h> // For JSON parsing (used in on_message_cb)
#include <time.h>    // 新增: For time() and srand()
#include <errno.h>   // For strerror(errno)


// 引入全局变量
extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running;
extern mqtt_device_config_t device_config; // 引入全局的 device_config

// --- 辅助函数 (模拟传感器数据获取和 JSON 构建) ---

// 构建符合华为云 IoTDA 属性上报格式的 JSON 字符串
// 关键：现在它重新接收 int hr_value 和 int spo2_value
void build_huawei_property_json(char *buffer, size_t size, int hr_value, int spo2_value) {
    // Huawei Cloud IoTDA property reporting format: {"services":[{"service_id":"your_service_id","properties":{"your_property_name":value}}]}
    // 这里使用 "mqtt" 作为 service_id，"HR" 和 "SpO2" 作为属性名。
    snprintf(buffer, size,
             "{\"services\":[{\"service_id\":\"mqtt\",\"properties\":{\"HR\":%d,\"SpO2\":%d}}]}",
             hr_value, spo2_value);
}


// --- Mosquitto Callbacks (Unified for single instance) ---

// Callback for successful connection
void on_connect_cb(struct mosquitto *mosq_obj, void *userdata, int result) {
    printf("DEBUG: on_connect_cb triggered with result: %d (%s)\n", result, mosquitto_connack_string(result));

    mqtt_device_config_t *cfg = (mqtt_device_config_t *)userdata; // 使用 userdata
    if (result == 0) {
        printf("MQTT: Connected to broker successfully.\n");
        mqtt_connected_flag = 1; // 设置连接成功标志

        printf("MQTT: Subscribing to topic: %s\n", cfg->subscribe_topic);

        printf("DEBUG: Subscribe Topic Hex: ");
        for (int i = 0; cfg->subscribe_topic[i] != '\0'; i++) {
            printf("%02X ", (unsigned char)cfg->subscribe_topic[i]);
        }
        printf("\n");

        int subscribe_rc = mosquitto_subscribe(mosq_obj, NULL, cfg->subscribe_topic, 1); // QoS 1 for reliability
        if (subscribe_rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "MQTT: Failed to initiate subscribe request: %s\n", mosquitto_strerror(subscribe_rc));
        } else {
            printf("MQTT: Subscribe request sent successfully to broker.\n");
        }
    } else {
        fprintf(stderr, "MQTT: Connection failed: %s\n", mosquitto_connack_string(result));
        mqtt_connected_flag = 0; // 连接失败
        if (result == MOSQ_ERR_PROTOCOL || result == MOSQ_ERR_ACL_DENIED) {
             keep_running = 0; // 致命错误或认证错误，优雅终止
        }
    }
}

// Callback for receiving messages
void on_message_cb(struct mosquitto *mosq_obj, void *userdata, const struct mosquitto_message *msg) {
    printf("\n--- Downlink message received ---\n");
    printf("Topic: %s\n", msg->topic);
    printf("Message: %.*s\n", msg->payloadlen, (char *)msg->payload);
    printf("------------------------------------------\n\n");

    if (global_dbus_conn) {
        char *ble_cmd_to_send = (char*)malloc(msg->payloadlen + 1);
        if (ble_cmd_to_send) {
            memcpy(ble_cmd_to_send, msg->payload, msg->payloadlen);
            ble_cmd_to_send[msg->payloadlen] = '\0';

            printf("Forwarding MQTT payload to BLE: \"%s\" to %s\n", ble_cmd_to_send, WRITABLE_CHARACTERISTIC_PATH);
            if (write_characteristic_value(global_dbus_conn, WRITABLE_CHARACTERISTIC_PATH, ble_cmd_to_send) < 0) {
                fprintf(stderr, "Failed to send BLE command to microcontroller.\n");
            }
            free(ble_cmd_to_send);
        } else {
            fprintf(stderr, "Memory allocation failed for BLE command.\n");
        }
    } else {
        fprintf(stderr, "D-Bus connection not available for BLE write.\n");
    }
}

// Callback for successful message publication
void on_publish_cb(struct mosquitto *mosq_obj, void *userdata, int mid) {
    printf("MQTT: Message published successfully, Message ID: %d\n", mid);
}

// Callback for successful subscription
void on_subscribe_cb(struct mosquitto *mosq_obj, void *userdata, int mid, int qos_count, const int *granted_qos) {
    printf("DEBUG: on_subscribe_cb triggered with mid: %d\n", mid);
    printf("MQTT: Topic subscribed successfully, Message ID: %d\n", mid);
    for (int i = 0; i < qos_count; i++) {
        printf("DEBUG: Granted QoS for topic %d: %d\n", mid, granted_qos[i]);
    }
}

// Callback for disconnection
void on_disconnect_cb(struct mosquitto *mosq_obj, void *userdata, int result) {
    printf("MQTT: Disconnected from broker, return code: %d\n", result);
    mqtt_connected_flag = 0; // 设置连接断开标志
}


// --- 下行线程函数 ---
void* downlink_thread_func(void* arg) {
    int rc;
    // time_t last_publish_time = 0; // 不再由本线程负责周期性发布模拟数据

    if (!global_mosq) {
        fprintf(stderr, "Downlink Thread: MQTT client not initialized in main thread.\n");
        return NULL;
    }

    printf("--- Downlink Thread: MQTT communication loop (for downlinks and connection management) ---\n");
    while (keep_running) {
        // Attempt to connect if not connected or if connection was lost
        rc = mosquitto_connect(global_mosq, device_config.host, device_config.port, device_config.keepalive_interval);
        printf("DEBUG: mosquitto_connect returned: %d (%s)\n", rc, mosquitto_strerror(rc));

        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Downlink Thread: Failed to connect to MQTT broker: %s. Retrying in 5 seconds...\n", mosquitto_strerror(rc));
            sleep(5);
            continue; // Skip the rest of the loop and try connecting again
        }

        // 关键：确保 on_connect_cb 被触发并处理初始事件
        printf("DEBUG: Connection initiated, waiting for on_connect_cb and initial events...\n");
        int loop_status_initial;
        int loop_attempts = 0;
        const int MAX_LOOP_ATTEMPTS = 10; // 尝试循环几次，给连接和订阅足够时间
        const int LOOP_TIMEOUT_MS = 100; // 每次循环的超时时间

        do {
            loop_status_initial = mosquitto_loop(global_mosq, LOOP_TIMEOUT_MS, 1);
            if (loop_status_initial != MOSQ_ERR_SUCCESS && loop_status_initial != MOSQ_ERR_NO_CONN) {
                fprintf(stderr, "DEBUG: Initial loop after connect encountered error: %s\n", mosquitto_strerror(loop_status_initial));
                break; // 遇到错误就退出内部循环
            }
            usleep(10000); // 10ms 睡眠，防止忙等待
            loop_attempts++;
        } while (loop_attempts < MAX_LOOP_ATTEMPTS && mqtt_connected_flag == 0 && keep_running); // 在连接成功前持续循环，并考虑外部停止信号

        if (mqtt_connected_flag == 0 && keep_running) { // 如果经过多次循环仍未连接成功且程序未被要求停止
            fprintf(stderr, "DEBUG: Initial connection/subscription loop timed out or failed, attempting full reconnect...\n");
            mosquitto_disconnect(global_mosq);
            sleep(1);
            continue; // 返回外层循环，重新尝试连接
        }
        printf("DEBUG: Finished initial loop after connect. Continuing main loop.\n");

        // 这个循环处理 MQTT 网络事件
        // 它会在连接丢失时返回 MOSQ_ERR_NO_CONN，允许重连逻辑介入
        while (mqtt_connected_flag && keep_running) { // 只有在连接成功且程序运行时才执行此循环
            rc = mosquitto_loop(global_mosq, 100, 1); // 100ms timeout
            if (rc != MOSQ_ERR_SUCCESS) {
                if (rc == MOSQ_ERR_NO_CONN) {
                    printf("Downlink Thread: Mosquitto loop reports no connection, breaking to reconnect.\n");
                    mqtt_connected_flag = 0; // 确保标志被清除
                } else {
                    fprintf(stderr, "Downlink Thread: Mosquitto loop error: %s. Attempting to reconnect...\n", mosquitto_strerror(rc));
                    mosquitto_disconnect(global_mosq); // 强制断开以触发重连
                }
                sleep(1); // 短暂延迟
                break; // 退出当前内部 while 循环，让外层循环尝试重新连接
            }
            // Downlink 线程主要负责 mosquitto_loop 和处理下行消息
            // 上行发布现在由 BLE 线程通过回调触发
            usleep(10000); // Small sleep to prevent busy-waiting (10ms)
        }
    }

    printf("Downlink Thread: Exiting...\n");
    return NULL;
}
