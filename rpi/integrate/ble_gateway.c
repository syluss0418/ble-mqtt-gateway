#include "ble_gateway.h"
#include "mqtt_gateway.h" // 包含 mqtt_gateway.h 来获取 mqtt_device_config_t 和 build_huawei_property_json

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h> // For usleep()
#include <json-c/json.h> // For JSON parsing (used in handle_properties_changed)
// #include <MQTTClient.h> // 移除：不再使用 Paho MQTT C 客户端库

// --- 辅助函数声明 (在 mqtt_gateway.c 中实现，但 ble_gateway.c 需要调用) ---
// 注意：build_huawei_property_json 的签名已更改为接受 HR 和 SpO2
// extern void build_huawei_property_json(char *buffer, size_t size, int hr_value, int spo2_value); // 已经通过 mqtt_gateway.h 包含
// extern mqtt_device_config_t device_config; // 已经通过 mqtt_gateway.h 包含


// --- BLE D-Bus 相关的实现函数 (从 iot_gateway.c 移动过来) ---

/* 调用 D-BUS 上的一个方法
 * 参数1：D-BUS 连接对象        参数2：目标 D-BUS 对象路径
 * 参数3：目标D-BUS 接口名      参数4：目标方法名
 * return 0 成功，-1 创建消息失败， -2 方法调用失败
 */
int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method)
{
    DBusMessage     *msg, *reply;
    DBusError       err;

    dbus_error_init(&err);

    //构建一个新方法调用消息
    msg = dbus_message_new_method_call(BLUEZ_BUS_NAME, path, interface, method);
    if (!msg)
    {
        fprintf(stderr, "Failed to create D-BUS message for method %s.\n", method);
        return -1;
    }

    //向D-BUS 发送消息，并阻塞等待应答
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);

    //释放发送的消息
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err))
    {
        fprintf(stderr, "D-BUS call %s on %s failed: %s\n", method, path, err.message);
        dbus_error_free(&err);
        return -2;
    }
    if (reply)
    {
        dbus_message_unref(reply); //释放收到的应答消息
    }

    return 0;
}

/* 打印接收到的通知值*/
// 修改此函数以返回解码后的字符串，以便在 handle_properties_changed 中解析
void print_notify_value(DBusMessageIter *variant_iter)
{
    DBusMessageIter array_iter;
    dbus_message_iter_recurse(variant_iter, &array_iter);

    printf("Received Notification Value: ");
    while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID)
    {
        uint8_t byte_val;
        dbus_message_iter_get_basic(&array_iter, &byte_val);
        printf("%02x ", byte_val);
        dbus_message_iter_next(&array_iter);
    }
    printf(" (Hex)\n");


    //转换为字符串打印
    printf("Decoded string: \"");
    dbus_message_iter_recurse(variant_iter, &array_iter); //重新初始化迭代器
    while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID)
    {
        uint8_t byte_val;
        dbus_message_iter_get_basic(&array_iter, &byte_val);
        if (byte_val >= 32 && byte_val <= 126)
        {
            printf("%c", (char)byte_val);
        }
        else
        {
            printf("."); //非可打印字符用点代替
        }
        dbus_message_iter_next(&array_iter);
    }
    printf("\"\n"); // 修正：添加结束引号
}

/* 辅助函数：从 D-Bus 变体迭代器中解析出字符串 */
// 这个函数将帮助我们获取原始通知字符串
static char* get_string_from_dbus_variant(DBusMessageIter *variant_iter) {
    DBusMessageIter array_iter;
    dbus_message_iter_recurse(variant_iter, &array_iter);

    // 计算字符串长度
    int len = 0;
    DBusMessageIter temp_iter = array_iter;
    while (dbus_message_iter_get_arg_type(&temp_iter) != DBUS_TYPE_INVALID) {
        len++;
        dbus_message_iter_next(&temp_iter);
    }

    char *str = (char*)malloc(len + 1); // +1 for null terminator
    if (!str) {
        fprintf(stderr, "Memory allocation failed for string from D-Bus variant.\n");
        return NULL;
    }

    int i = 0;
    dbus_message_iter_recurse(variant_iter, &array_iter); // Reset iterator
    while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID) {
        uint8_t byte_val;
        dbus_message_iter_get_basic(&array_iter, &byte_val);
        str[i++] = (char)byte_val;
        dbus_message_iter_next(&array_iter);
    }
    str[len] = '\0'; // Null-terminate the string

    return str;
}


/* 处理PropertiesChanged 信号，提取并打印特性值 */
// 修改：现在它会发布 MQTT 消息，并解析 HR/SpO2
void handle_properties_changed(DBusMessage *msg)
{
    DBusMessageIter         args;
    const char                      *iface;
    DBusMessageIter         changed_props;
    DBusMessageIter         entry;
    const char                      *key;
    DBusMessageIter         variant_iter;

    //初始化迭代器，指向消息的第一个参数
    dbus_message_iter_init(msg, &args);

    //读取第一个参数：接口名（string）
    dbus_message_iter_get_basic(&args, &iface);
    dbus_message_iter_next(&args);

    //递归进入第二个参数：changed_props字典
    dbus_message_iter_recurse(&args, &changed_props);

    //遍历changed_props字典项
    while (dbus_message_iter_get_arg_type(&changed_props) != DBUS_TYPE_INVALID)
    {
        dbus_message_iter_recurse(&changed_props, &entry); //进入字典的键值对

        dbus_message_iter_get_basic(&entry, &key); //读取键（string）

        if(strcmp(key, "Value") == 0) //如果键是”Value“，说明特性值发生了变化
        {
            dbus_message_iter_next(&entry); //移动到值
            dbus_message_iter_recurse(&entry, &variant_iter); //递归进入变体

            printf("---Notification received from %s---\n", dbus_message_get_path(msg));
            print_notify_value(&variant_iter); //打印通知值

            // --- 新增：将接收到的通知数据通过 MQTT 发布到华为云 ---
            if (mqtt_connected_flag) {
                char *decoded_str = get_string_from_dbus_variant(&variant_iter);
                if (decoded_str) {
                    int hr = 0, spo2 = 0;
                    // 尝试从字符串中解析 HR 和 SpO2，例如 "HR:067,SpO2:099"
                    // 注意：sscanf 返回成功匹配的项数
                    if (sscanf(decoded_str, "HR:%d,SpO2:%d", &hr, &spo2) == 2) {
                        printf("Parsed HR: %d, SpO2: %d\n", hr, spo2);

                        char json_payload_buffer[256];
                        // 调用修改后的 build_huawei_property_json
                        build_huawei_property_json(json_payload_buffer, sizeof(json_payload_buffer), hr, spo2);

                        printf("Publishing MQTT payload: %s\n", json_payload_buffer);

                        // 使用 mosquitto_publish，而不是 MQTTClient_publishMessage
                        int rc_pub = mosquitto_publish(global_mosq, NULL, device_config.publish_topic, strlen(json_payload_buffer), json_payload_buffer, 1, false);
                        if (rc_pub != MOSQ_ERR_SUCCESS) {
                            fprintf(stderr, "Failed to publish MQTT message, return code %d\n", rc_pub);
                        } else {
                            printf("MQTT message published successfully.\n");
                        }
                    } else {
                        fprintf(stderr, "Failed to parse HR and SpO2 from notification string: \"%s\"\n", decoded_str);
                    }
                    free(decoded_str); // 释放 malloc 的内存
                } else {
                    fprintf(stderr, "Failed to get decoded string from D-Bus variant.\n");
                }
            } else {
                fprintf(stderr, "MQTT not connected, cannot publish BLE notification.\n");
            }
            printf("-----------------------------------\n");
        }
        dbus_message_iter_next(&changed_props); //移动到下一个字典项
    }
}

/* 向BLE特征值写入数据 */
int write_characteristic_value(DBusConnection *conn, const char *char_path, const char *cmd_str)
{
    DBusMessage             *msg;
    DBusMessageIter args, array_iter, options_iter;
    DBusError               err;

    dbus_error_init(&err);

    //创建方法调用消息：org.bluez.GattCharacteristic1.WriteValue
    msg = dbus_message_new_method_call(BLUEZ_BUS_NAME, char_path, "org.bluez.GattCharacteristic1", "WriteValue");
    if (!msg) // 修正：如果 msg 是 NULL (即创建失败)，则进入此块
    {
        fprintf(stderr, "Failed to create D-BUS message for writevalue.\n");
        return -1;
    }

    // 初始化参数迭代器，用于构造 WriteValue() 的参数
    dbus_message_iter_init_append(msg, &args);

    // 添加第一个参数：要写入的字节数组 (ARRAY of BYTE 'y')
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "y", &array_iter);
    for (size_t i = 0; i < strlen(cmd_str); ++i)
    {
        uint8_t b = (uint8_t)cmd_str[i];
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &b);
    }
    dbus_message_iter_close_container(&args, &array_iter);

    // 添加第二个参数：选项字典 (ARRAY of DICT_ENTRY {string, variant})
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options_iter);
    dbus_message_iter_close_container(&args, &options_iter);

    // 同步发送消息，等待回复
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg); // 释放发送的消息

    if (dbus_error_is_set(&err))
    {
        fprintf(stderr, "WriteValue failed for %s: %s\n", char_path, err.message);
        dbus_error_free(&err);
        return -1;
    }

    printf("Successfully sent: \"%s\" to %s\n", cmd_str, char_path);
    if (reply) {
        dbus_message_unref(reply); // 释放回复消息对象
    }
    return 0;
}

// --- 上行线程函数 ---
void* uplink_thread_func(void* arg) {
    DBusError err;
    dbus_error_init(&err);

    printf("Uplink Thread: Starting BLE operations...\n");

    // 1. 连接到 BLE 设备
    printf("Uplink Thread: Connecting to BLE device %s...\n", DEVICE_MAC);
    if (call_method(global_dbus_conn, DEVICE_PATH, "org.bluez.Device1", "Connect") < 0) {
        fprintf(stderr, "Uplink Thread: Failed to connect to BLE device.\n");
        return NULL;
    }
    printf("Uplink Thread: Successfully connected to BLE devices.\n");

    // 2. 启用特性通知
    printf("Uplink Thread: Enabling notifications for characteristic %s...\n", NOTIFY_CHARACTERISTIC_PATH);
    if (call_method(global_dbus_conn, NOTIFY_CHARACTERISTIC_PATH, "org.bluez.GattCharacteristic1", "StartNotify") < 0) {
        fprintf(stderr, "Uplink Thread: Failed to enable notification.\n");
        return NULL;
    }
    printf("Uplink Thread: Notification enabled.\n");

    // 3. 添加 D-Bus 信号匹配规则 (确保格式正确)
    dbus_bus_add_match(global_dbus_conn, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Uplink Thread: D-BUS Match Rule Error: %s\n", err.message);
        dbus_error_free(&err);
        return NULL;
    }
    dbus_connection_flush(global_dbus_conn);
    printf("Uplink Thread: D-BUS signal match rule added for notification.\n");

    printf("\n--- Uplink Thread: Entering communication loop ---\n");
    printf("Uplink Thread: Listening for notifications.\n");

    while (keep_running) {
        // 处理 D-Bus I/O (接收 BLE 信号和回复)
        dbus_connection_read_write(global_dbus_conn, 100);

        // 从 D-Bus 消息队列中弹出并处理消息
        DBusMessage *msg = dbus_connection_pop_message(global_dbus_conn);
        if (msg) {
            // 判断是否是 BLE 的通知信号 (PropertiesChanged)
            if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged") &&
                strstr(dbus_message_get_path(msg), NOTIFY_CHARACTERISTIC_PATH)) {
                // handle_properties_changed 会在内部发布 MQTT 消息
                handle_properties_changed(msg);
            }
            dbus_message_unref(msg);
        }

        usleep(100000); // 100 毫秒延时
    }

    printf("Uplink Thread: Exiting...\n");
    return NULL;
}

