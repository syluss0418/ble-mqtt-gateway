#ifndef MQTT_GATEWAY_H
#define MQTT_GATEWAY_H

#include <mosquitto.h> // Include Mosquitto library for struct mosquitto
#include <stddef.h>    // For size_t

// Forward declarations for global variables used by mqtt_gateway.c
// These are defined in main.c
extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running; // For graceful shutdown

// MQTT Device Configuration Structure
// Defined in main.c and extern here for use in mqtt_gateway.c and ble_gateway.c
typedef struct {
    const char *host;
    int         port;
    const char *client_id;
    const char *username;
    const char *password;
    const char *publish_topic;
    const char *subscribe_topic;
    int         keepalive_interval;
    int         publish_interval_sec;
} mqtt_device_config_t;

extern mqtt_device_config_t device_config;


// --- Mosquitto Callbacks ---
void on_connect_cb(struct mosquitto *mosq, void *userdata, int result);
void on_message_cb(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg);
void on_publish_cb(struct mosquitto *mosq, void *userdata, int mid);
void on_subscribe_cb(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos);
void on_disconnect_cb(struct mosquitto *mosq, void *userdata, int result);

// --- Thread Functions ---
void* downlink_thread_func(void* arg); // MQTT communication thread (subscribe & connection management)

// --- Helper Functions ---
// 关键：现在它再次接受 int hr_value 和 int spo2_value
void build_huawei_property_json(char *buffer, size_t size, int hr_value, int spo2_value); // <-- 再次确认这个签名

#endif // MQTT_GATEWAY_H
