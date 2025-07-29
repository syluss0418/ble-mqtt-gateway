#include <stdio.h>
#include <mosquitto.h> // Include Mosquitto library
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <signal.h>  // For signal handling

// Global flag to control program execution and graceful shutdown
volatile int keep_running = 1;

// Signal handler for SIGINT (Ctrl+C)
void sigint_handler(int signum) {
    printf("\nCaptured SIGINT signal (%d). Setting exit flag for graceful shutdown...\n", signum);
    keep_running = 0; // Set the global flag to 0 to stop the main loop
}

// Global Mosquitto instance pointer, managed by main function
struct mosquitto *mosq = NULL;

// MQTT Device Configuration Structure
typedef struct {
    const char *host;
    int         port;
    const char *client_id;       // Huawei Cloud specified Client ID
    const char *username;
    const char *password;
    const char *publish_topic;
    const char *subscribe_topic;
    int         keepalive_interval;
    int         publish_interval_sec;
} mqtt_device_config_t;

// Your Device Configuration (Please replace with your actual device info)
// This structure will be used as the base configuration.
mqtt_device_config_t device_config = {
    .host = "5969442708.st1.iotda-device.cn-north-4.myhuaweicloud.com",
    .port = 1883,
    // IMPORTANT: This client_id must be the full string Huawei Cloud expects for your device.
    // === 请务必在这里核对并精确复制华为云平台的 Client ID ===
    .client_id = "687ca704d582f200183d3b33_040210_0_0_2025072904",
    .username = "687ca704d582f200183d3b33_040210",
    // === 请务必在这里核对并精确复制华为云平台的 Password ===
    .password = "327d73c2c112fd381f62dcb84728873d9a3f3ef7aa96d7ed8ba7de9befba24c7",
    .publish_topic = "$oc/devices/687ca704d582f200183d3b33_040210/sys/properties/report",
    // === 请务必在这里核对并精确复制华为云平台的下行订阅主题 ===
    .subscribe_topic = "$oc/devices/687ca704d582f200183d3b33_040210/sys/messages/down",
    .keepalive_interval = 60,
    .publish_interval_sec = 5
};

/* Mosquitto Callbacks (Unified for single instance) */

// Callback for successful connection
void on_connect_cb(struct mosquitto *mosq_obj, void *userdata, int result) {
    // === DEBUG PRINT: Always print when on_connect_cb is triggered ===
    printf("DEBUG: on_connect_cb triggered with result: %d (%s)\n", result, mosquitto_connack_string(result));

    mqtt_device_config_t *cfg = (mqtt_device_config_t *)userdata;
    if (result == 0) {
        printf("MQTT: Connected to broker successfully.\n");
        printf("MQTT: Subscribing to topic: %s\n", cfg->subscribe_topic);

        // === DEBUG PRINT: Print hex representation of subscribe topic ===
        printf("DEBUG: Subscribe Topic Hex: ");
        for (int i = 0; cfg->subscribe_topic[i] != '\0'; i++) {
            printf("%02X ", (unsigned char)cfg->subscribe_topic[i]);
        }
        printf("\n");
        // ================================================================

        int subscribe_rc = mosquitto_subscribe(mosq_obj, NULL, cfg->subscribe_topic, 1); // QoS 1 for reliability
        if (subscribe_rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "MQTT: Failed to initiate subscribe request: %s\n", mosquitto_strerror(subscribe_rc));
        } else {
            printf("MQTT: Subscribe request sent successfully to broker.\n");
        }
    } else {
        fprintf(stderr, "MQTT: Connection failed: %s\n", mosquitto_connack_string(result));
        // If connection fails due to protocol error, terminate. Other errors might retry.
        // For authentication denied, connection will be closed by broker (rc=5 for mosquitto_connect, then on_disconnect_cb)
        if (result == MOSQ_ERR_PROTOCOL || result == MOSQ_ERR_ACL_DENIED) {
             keep_running = 0; // Fatal error or auth error, terminate gracefully
        }
    }
}

// Callback for receiving messages
void on_message_cb(struct mosquitto *mosq_obj, void *userdata, const struct mosquitto_message *msg) {
    printf("\n--- Downlink message received ---\n");
    printf("Topic: %s\n", msg->topic);
    printf("Message: %.*s\n", msg->payloadlen, (char *)msg->payload);
    printf("------------------------------------------\n\n");
    // Add your logic here to process the received message (e.g., parse JSON, control devices)
}

// Callback for successful message publication
void on_publish_cb(struct mosquitto *mosq_obj, void *userdata, int mid) {
    printf("MQTT: Message published successfully, Message ID: %d\n", mid);
}

// Callback for successful subscription
void on_subscribe_cb(struct mosquitto *mosq_obj, void *userdata, int mid, int qos_count, const int *granted_qos) {
    // === DEBUG PRINT: Always print when on_subscribe_cb is triggered ===
    printf("DEBUG: on_subscribe_cb triggered with mid: %d\n", mid);
    printf("MQTT: Topic subscribed successfully, Message ID: %d\n", mid);
    // You can also check granted_qos here for each topic subscribed (if subscribing to multiple)
    for (int i = 0; i < qos_count; i++) {
        printf("DEBUG: Granted QoS for topic %d: %d\n", mid, granted_qos[i]);
    }
}

// Callback for disconnection
void on_disconnect_cb(struct mosquitto *mosq_obj, void *userdata, int result) {
    printf("MQTT: Disconnected from broker, return code: %d\n", result);
    // The main loop will attempt to reconnect if keep_running is true
}

// Simulates fetching sensor data (temperature)
static void get_simulated_temperature(float *temp_value) {
    *temp_value = 25.0f + (rand() % 100) / 10.0f; // Generates a random temperature between 25.0 and 34.9
}

// Builds a JSON string conforming to Huawei Cloud IoTDA's property reporting format
static void build_huawei_property_json(char *buffer, size_t size, float temp_value) { // Corrected function name
    // Huawei Cloud IoTDA property reporting format: {"services":[{"service_id":"your_service_id","properties":{"your_property_name":value}}]}
    // Here, "mqtt" is used as service_id, and "temp" as property name.
    snprintf(buffer, size,
             "{\"services\":[{\"service_id\":\"mqtt\",\"properties\":{\"temp\":%.2f}}]}",
             temp_value);
}

int main(int argc, char **argv) {
    // No command line arguments will be parsed in this version.
    // Use the global device_config directly.

    srand(time(NULL)); // Initialize random number generator
    signal(SIGINT, sigint_handler); // Register signal handler for SIGINT (Ctrl+C)

    mosquitto_lib_init();
    printf("Main: Mosquitto library initialized.\n");

    // Create a single Mosquitto client instance
    // Pass device_config as userdata to callbacks
    mosq = mosquitto_new(device_config.client_id, true, (void*)&device_config); // Use global device_config
    if (!mosq) {
        fprintf(stderr, "Main: Failed to create Mosquitto instance: %s\n", strerror(errno));
        mosquitto_lib_cleanup();
        return 1;
    }
    printf("Main: Mosquitto client instance created with Client ID: %s\n", device_config.client_id);

    // Set all callbacks for the single instance
    mosquitto_connect_callback_set(mosq, on_connect_cb);
    mosquitto_message_callback_set(mosq, on_message_cb);
    mosquitto_publish_callback_set(mosq, on_publish_cb);
    mosquitto_subscribe_callback_set(mosq, on_subscribe_cb);
    mosquitto_disconnect_callback_set(mosq, on_disconnect_cb);

    // Set username and password
    int rc = mosquitto_username_pw_set(mosq, device_config.username, device_config.password);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Main: Failed to set username/password: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    // Set MQTT protocol version to 3.1.1 (Recommended by Huawei Cloud)
    int protocol_version = MQTT_PROTOCOL_V311; // Define an int variable
    rc = mosquitto_opts_set(mosq, MOSQ_OPT_PROTOCOL_VERSION, &protocol_version); // Pass address of int
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Main: Failed to set MQTT protocol version: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    printf("Main: Program is running. Press Ctrl+C to exit.\n");

    time_t last_publish_time = 0; // To track when last message was published

    while (keep_running) {
        // Attempt to connect if not connected or if connection was lost
        // mosquitto_connect will return MOSQ_ERR_SUCCESS if already connected, no need for mosquitto_conn_peer
        rc = mosquitto_connect(mosq, device_config.host, device_config.port, device_config.keepalive_interval);
        // === DEBUG PRINT: Print the return code of mosquitto_connect ===
        printf("DEBUG: mosquitto_connect returned: %d (%s)\n", rc, mosquitto_strerror(rc)); 

        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Main: Failed to connect to MQTT broker: %s. Retrying in 5 seconds...\n", mosquitto_strerror(rc));
            sleep(5);
            continue; // Skip the rest of the loop and try connecting again
        }

        // --- NEW LOGIC ADDED HERE: Ensure on_connect_cb is triggered after successful mosquitto_connect ---
        if (rc == MOSQ_ERR_SUCCESS) {
            printf("DEBUG: Connection initiated, waiting for on_connect_cb and initial events...\n");
            // Run loop for a short period to allow connection to complete and callbacks to fire
            int loop_status_initial;
            int loop_attempts = 0;
            const int MAX_LOOP_ATTEMPTS = 10; // Try to loop a few more times
            const int LOOP_TIMEOUT_MS = 100; // Short timeout for each loop iteration

            do {
                loop_status_initial = mosquitto_loop(mosq, LOOP_TIMEOUT_MS, 1);
                // We're expecting on_connect_cb to trigger and potentially on_subscribe_cb
                // if connection is established and subscribe request is processed.
                if (loop_status_initial != MOSQ_ERR_SUCCESS && loop_status_initial != MOSQ_ERR_NO_CONN) {
                    fprintf(stderr, "DEBUG: Initial loop after connect encountered error: %s\n", mosquitto_strerror(loop_status_initial));
                    break; // Exit this inner loop on error
                }
                usleep(10000); // 10ms sleep to prevent busy-waiting
                loop_attempts++;
            } while (loop_attempts < MAX_LOOP_ATTEMPTS); // Keep looping for a defined number of attempts

            if (loop_status_initial != MOSQ_ERR_SUCCESS && loop_status_initial != MOSQ_ERR_NO_CONN) {
                // If initial loops failed critically, disconnect and let outer loop retry connection
                fprintf(stderr, "DEBUG: Initial connection/subscription loop failed, attempting full reconnect...\n");
                mosquitto_disconnect(mosq); 
                sleep(1);
                continue; // Go to outer while loop to retry connection
            }
            printf("DEBUG: Finished initial loop after connect. Continuing main loop.\n");
        }
        // --- END OF NEW LOGIC ---

        // This loop handles ongoing MQTT network events and periodic publishing
        // It will return MOSQ_ERR_NO_CONN if connection is lost, allowing re-connection logic
        rc = mosquitto_loop(mosq, 100, 1); 
        if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) { 
            fprintf(stderr, "Main: Mosquitto loop error: %s. Attempting to reconnect...\n", mosquitto_strerror(rc));
            mosquitto_disconnect(mosq); // Force disconnect to trigger re-connection logic
            sleep(1); // Small delay before next loop iteration tries to reconnect
            continue;
        }

        // Publish data periodically only if connected
        time_t current_time = time(NULL);
        if (current_time - last_publish_time >= device_config.publish_interval_sec) {
            // Check if mosq is actually connected (implied by mosquitto_connect not failing and loop not returning NO_CONN)
            // It's possible mosq is not truly connected if mosquitto_loop() failed silently or on_connect_cb didn't fire.
            // But if previous checks passed, it should be connected.
            if (mosq) { 
                float current_temperature;
                char json_payload_buffer[256];
                get_simulated_temperature(&current_temperature);
                build_huawei_property_json(json_payload_buffer, sizeof(json_payload_buffer), current_temperature);

                printf("Main: Preparing to publish data: %s\n", json_payload_buffer);

                int pub_ret = mosquitto_publish(mosq, NULL, device_config.publish_topic, strlen(json_payload_buffer), json_payload_buffer, 1, false);
                if (pub_ret != MOSQ_ERR_SUCCESS) {
                    fprintf(stderr, "Main: Failed to publish message: %s\n", mosquitto_strerror(pub_ret));
                    // Publishing failed, might be connection issue, loop will detect and try to reconnect
                } else {
                    last_publish_time = current_time; // Only update on successful publish
                    // on_publish_cb will print "MQTT: Message published successfully, Message ID: %d\n"
                    // So we don't need a redundant printf here.
                }
            } else {
                fprintf(stderr, "Main: Mosquitto instance is null, cannot publish.\n");
            }
        }
        usleep(10000); // Small sleep to prevent busy-waiting (10ms)
    }

    printf("Main: Received exit signal, cleaning up MQTT client...\n");
    if (mosq) {
        mosquitto_disconnect(mosq); // Clean disconnect
        mosquitto_destroy(mosq);    // Free the Mosquitto instance
    }

    mosquitto_lib_cleanup();
    printf("Main: Mosquitto library cleaned up.\n");

    return 0;
}
