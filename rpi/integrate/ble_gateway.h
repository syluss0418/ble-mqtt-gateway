#ifndef __BLE_GATEWAY_H
#define __BLE_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>
#include <dbus/dbus.h>     // D-Bus for BLE communication
#include <mosquitto.h>     // Mosquitto library for MQTT (因为 handle_properties_changed 会用到)

// --- D-Bus 常量定义 (从 iot_gateway.c 移动过来) ---
#define BLUEZ_BUS_NAME                                "org.bluez"
#define ADAPTER_PATH                                  "/org/bluez/hci0" // 假设适配器是 hci0

// --- 您的 ESP32 设备信息 (从 iot_gateway.c 移动过来) ---
// 请替换为您的 ESP32 实际的 MAC 地址，注意将冒号替换为下划线
#define DEVICE_MAC                             "A0_76_4E_57_E9_62"
#define DEVICE_PATH                            "/org/bluez/hci0/dev_" DEVICE_MAC

// --- GATT 特征值 D-Bus 对象路径定义 (从 iot_gateway.c 移动过来) ---
// 这些路径是根据您之前的 `gatttool char-desc` 和 `AT+BLEGATTSCHAR?` 输出推断的
// 服务句柄 0x0028 (UUID 0000a002-...)
// 通知特性声明句柄 0x0037 (UUID 0000c305-...)
// 通知特性 CCCD 句柄 0x0039 (UUID 00002902-...)
// 可写特性声明句柄 0x002f (UUID 0000c302-...)
// 可写特性值句柄 0x0030

// 用于启用通知的特性对象路径 (使用特性声明句柄)
#define NOTIFY_CHARACTERISTIC_PATH             DEVICE_PATH "/service0028/char0037"
// 用于写入数据的特性对象路径 (使用特性声明句柄)
#define WRITABLE_CHARACTERISTIC_PATH           DEVICE_PATH "/service0028/char002f"

// --- 全局变量声明 (在 main.c 中定义，这里声明为 extern) ---
extern DBusConnection *global_dbus_conn;
extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running;

// --- 函数原型 ---
void* uplink_thread_func(void* arg); // 上行线程函数
int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method);
void print_notify_value(DBusMessageIter *variant_iter);
void handle_properties_changed(DBusMessage *msg);
int write_characteristic_value(DBusConnection *conn, const char* char_path, const char* cmd_str);

#endif // __BLE_GATEWAY_H

