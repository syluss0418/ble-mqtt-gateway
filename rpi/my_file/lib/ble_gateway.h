#ifndef __BLE_GATEWAY_H
#define __BLE_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>
#include <dbus/dbus.h>
#include <mosquitto.h>


/* ---D-Bus 常量定义--- */
#define BLUEZ_BUS_NAME	"org.bluez"
#define ADAPTER_PATH	"/org/bluez/hci0"


/* ---ESP32 设备信息--- */
#define DEVICE_MAC		"A0_76_4E_57_E9_62"
#define DEVICE_PATH     "/org/bluez/hci0/dev_" DEVICE_MAC


/* ---GATT 特征值D-Bus 对象路径--- */

//用于启用通知的特性对象路径（ESP32->rpi）
#define NOTIFY_CHARACTERISTIC_PATH		DEVICE_PATH "/service0028/char0037"
//用于写入数据的特性对象路径（rpi->ESP32）
#define WRITABLE_CHARACTERISTIC_PATH    DEVICE_PATH "/service0028/char002f"


extern DBusConnection *global_dbus_conn;
extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running;


void *uplink_thread_func(void *arg);
int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method);
void print_notify_value(DBusMessageIter *variant_iter);
void handle_properties_changed(DBusMessage *msg);
int write_characteristic_value(DBusConnection *conn, const char* char_path, const char* cmd_str);
void print_notify_value(DBusMessageIter *variant_iter);


#endif // __BLE_GATEWAY_H
