#ifndef __BLE_GATEWAY_H
#define __BLE_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>
#include <dbus/dbus.h>
#include <mosquitto.h>


/* ---D-Bus 常量定义--- */
#define BLUEZ_BUS_NAME	"org.bluez"
#define ADAPTER_PATH	"/org/bluez/hci0"


/* --- ESP32 设备信息 & GATT 特征值D-Bus 对象路径 --- */
extern char BLE_DEVICE_MAC[32];
extern char DEVICE_PATH[256];
extern char NOTIFY_CHARACTERISTIC_PATH[512];
extern char WRITABLE_CHARACTERISTIC_PATH[512];


/* --- 业务逻辑阈值 --- */
extern int HR_THRESHOLD;
extern int SPO2_THRESHOLD;
extern char WARNING_CMD[128];

extern DBusConnection 	*global_dbus_conn;
extern struct 			mosquitto *global_mosq;
extern volatile int 	mqtt_connected_flag;
extern volatile int 	keep_running;


void *uplink_thread_func(void *arg);
int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method);
void print_notify_value(DBusMessageIter *variant_iter);
void handle_properties_changed(DBusMessage *msg);
int write_characteristic_value(DBusConnection *conn, const char* char_path, const char* cmd_str);
void print_notify_value(DBusMessageIter *variant_iter);

#endif // __BLE_GATEWAY_H
