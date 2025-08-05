#ifndef MQTT_GATEWAY_H
#define MQTT_GATEWAY_H

#include <mosquitto.h> // Include Mosquitto library for struct mosquitto
#include <stddef.h>    // For size_t

extern struct mosquitto *global_mosq;
extern volatile int mqtt_connected_flag;
extern volatile int keep_running; // For graceful shutdown

// MQTT Device Configuration Structure
typedef struct {
    char		*host;
    int         port;
    char		*client_id;
    char		*username;
    char		*password;
    char		*publish_topic;
    char		*subscribe_topic;
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
void build_huawei_property_json(char *buffer, size_t size, int hr_value, int spo2_value);

#endif // MQTT_GATEWAY_H
