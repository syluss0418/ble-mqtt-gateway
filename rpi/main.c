#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <libgen.h>
#include <limits.h> 
#include <sys/types.h>
#include <sys/stat.h>

#include <dbus/dbus.h>
#include <mosquitto.h>
#include <json-c/json.h>

#include "ble_gateway.h"
#include "mqtt_gateway.h"
#include "config_parser.h"
#include "pidfile.h"
#include "log.h"

// D-Bus连接对象
DBusConnection *global_dbus_conn = NULL;
// Mosquitto客户端实例
struct mosquitto *global_mosq = NULL;
// MQTT连接状态（0 未连接，1 已连接）
volatile int mqtt_connected_flag = 0;
// 程序运行控制标志
volatile int keep_running = 1;

// 互斥锁
pthread_mutex_t dbus_mutex;
pthread_mutex_t mqtt_mutex;

// 全局配置变量
// MQTT 配置
mqtt_device_config_t device_config;
// BLE 配置
char BLE_DEVICE_MAC[32];
char DEVICE_PATH[256];
char NOTIFY_CHARACTERISTIC_PATH[512];
char WRITABLE_CHARACTERISTIC_PATH[512];
int HR_THRESHOLD;
int SPO2_THRESHOLD;
char WARNING_CMD[128];

// PID文件路径
static char pid_file_path[PATH_MAX] = "./iot_gateway.pid";

void cleanup_config();

// 清理函数，将在程序退出时自动调用
void cleanup_handler()
{
    // 如果 PID 文件路径已设置，则删除它
    if (pid_file_path[0] != '\0')
    {
        remove_pid_file(pid_file_path);
    }

    // 释放由strdup分配的内存
    cleanup_config();

    // 释放其他资源
    if (global_dbus_conn)
    {
        dbus_connection_unref(global_dbus_conn);
    }
    if (global_mosq)
    {
        mosquitto_destroy(global_mosq);
    }
    mosquitto_lib_cleanup();

    log_info("Gateway application exited gracefully.\n");
    log_close();
}

// 信号处理函数
void sigint_handler(int signum)
{
    log_debug("Captured signal (%d). Setting exit flag for graceful shutdown...\n", signum);
    keep_running = 0;
}

void print_usage(char *progname)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n", progname);
    fprintf(stderr, "-c(--config): Specify the path to the configuration file.\n");
    fprintf(stderr, "-d(--daemon): Set program running on background.\n");
    fprintf(stderr, "-l(--log): Set program to verbose output mode (print logs to stdout).\n");
    fprintf(stderr, "-h(--help): Display this help information.\n");
    return;
}

// 释放由strdup分配的内存
void cleanup_config()
{
    if (device_config.host) free(device_config.host);
    if (device_config.client_id) free(device_config.client_id);
    if (device_config.username) free(device_config.username);
    if (device_config.password) free(device_config.password);
    if (device_config.publish_topic) free(device_config.publish_topic);
    if (device_config.subscribe_topic) free(device_config.subscribe_topic);
}

int main(int argc, char **argv)
{
    pthread_t      uplink_tid; //上行线程ID
    pthread_t      downlink_tid; //下行线程ID
    DBusError      err;
    int            rc;
    char           *progname = NULL;
    int            daemon_run = 0; //默认非后台运行
    char           *config_file = NULL;
    char           log_file[PATH_MAX] = {0};
    int            log_size = 1024;
    int            log_level = LOG_LEVEL_INFO;
    int            ch;
    int            pid_rc;
    char           absolute_config_path[PATH_MAX] = {0}; // 用于存储配置文件的绝对路径

    struct option opts[] = {
        {"config", required_argument, NULL, 'c'},
        {"daemon", no_argument, NULL, 'd'},
        {"log", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    progname = basename(argv[0]);

    while((ch = getopt_long(argc, argv, "c:dlh", opts, NULL)) != -1)
    {
        switch(ch)
        {
            case 'c':
                config_file = optarg;
                break;
            case 'd':
                daemon_run = 1;
                strcpy(log_file, "./iot_gateway.log");
                break;
            case 'l':
                log_level = LOG_LEVEL_DEBUG;
                strcpy(log_file, "console");
                break;
            case 'h':
                print_usage(progname);
                return EXIT_SUCCESS;
            default:
                print_usage(progname);
                return EXIT_FAILURE;
        }
    }
    
    log_open(log_file[0] == '\0' ? "console" : log_file, log_level, log_size, LOG_LOCK_ENABLE);

    log_info("Main: Starting BLE-MQTT Gateway application...\n");
    
    // 检查配置文件是否已指定
    if(!config_file)
    {
        log_error("Error: Configuration file is not specified. Use -c or --config option.\n");
        return -1;
    }
    
    // 在守护进程化前解析配置文件
    if (realpath(config_file, absolute_config_path) == NULL) 
	{
        log_error("Error: Failed to get real path for config file '%s'.\n", config_file);
        return -1;
    }
    
    if(parse_json_config(absolute_config_path) != 0)
    {
        log_error("Error: Failed to parse JSON configuration file '%s'.\n", absolute_config_path);
        return -1;
    }
    
    log_info("Main: Configuration loaded successfully.\n");
    
    // 如果是守护进程模式，手动创建守护进程
    if(daemon_run)
    {
        // Fork a child process
        pid_t pid = fork();
        if (pid < 0) 
		{
            log_error("Error: Failed to fork.\n");
            return -1;
        }
        
        // Parent process exits immediately
        if (pid > 0) 
		{
            log_info("Main: Forked child process with PID %d. Parent exiting.\n", pid);
            exit(EXIT_SUCCESS); 
        }

        // Child process continues from here
        if (setsid() < 0) 
		{
            log_error("Error: Failed to create new session.\n");
            return -1;
        }
        
        // 将标准文件描述符重定向到 /dev/null
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }
    
    // 检查是否有其他实例正在运行，只在主进程（或守护进程）中执行
    pid_rc = create_pid_file(pid_file_path);
    if (pid_rc == PIDFILE_EXISTS_ERROR)
    {
        log_error("Error: Another instance of the program is already running. Exiting.\n");
        return -1;
    }
    else if (pid_rc != PIDFILE_SUCCESS)
    {
        log_error("Error: Failed to create PID file: %d. Exiting.\n", pid_rc);
        return -1;
    }

    // 注册退出清理函数
    if (atexit(cleanup_handler) != 0) 
	{
        log_error("Error: Failed to register exit handler.\n");
        return -1;
    }
    
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    
    log_info("Main: BLE-MQTT Gateway application is running. Press Ctrl+C to exit.\n");

    pthread_mutex_init(&dbus_mutex, NULL);
    pthread_mutex_init(&mqtt_mutex, NULL);

    //step 1:初始化D-Bus连接
    dbus_error_init(&err);
    //获取系统D-Bus总线连接
    global_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if(dbus_error_is_set(&err))
    {
        log_error("Main: Initial D-Bus connection error: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    log_info("Main: D-Bus system bus connected.\n");

    //step 2:初始化mosquitto 库和客户端实例
    mosquitto_lib_init();
    log_info("Main: Mosquitto library initialized.\n");

    global_mosq = mosquitto_new(device_config.client_id, true, (void *)&device_config);
    if(!global_mosq)
    {
        log_error("Main: Failed to create Mosquitto instance: %s\n", strerror(errno));
        return -2;
    }
    log_info("Main: Mosquitto client instance created with Client ID: %s\n", device_config.client_id);

    // 设置所有 MQTT 回调函数
    mosquitto_connect_callback_set(global_mosq, on_connect_cb);
    mosquitto_message_callback_set(global_mosq, on_message_cb);
    mosquitto_publish_callback_set(global_mosq, on_publish_cb);
    mosquitto_subscribe_callback_set(global_mosq, on_subscribe_cb);
    mosquitto_disconnect_callback_set(global_mosq, on_disconnect_cb);

    // 设置用户名和密码，用于MQTT代理的认证
    rc = mosquitto_username_pw_set(global_mosq, device_config.username, device_config.password);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        log_error("Main: Failed to set username/password: %s\n", mosquitto_strerror(rc));
        return -1;
    }

    //使用TLS/SSL加密连接
    if(device_config.ca_cert)
    {
        rc = mosquitto_tls_set(global_mosq,
                               device_config.ca_cert,
                               NULL, //client_cert_path
                               NULL, //client_key_path
                               NULL, //psk
                               NULL); //psk_identify

        if(rc != MOSQ_ERR_SUCCESS)
        {
            log_error("Main: Failed to set TLS options: %s\n", mosquitto_strerror(errno));
            return -1;
        }
    }

    // step 3:创建上行线程 (BLE 通知 -> MQTT 发布)
    if (pthread_create(&uplink_tid, NULL, uplink_thread_func, NULL) != 0)
    {
        log_error("Main: Failed to create uplink thread.\n");
        return -1;
    }
    log_debug("Main: Uplink thread created.\n");

    // step 4: 创建下行线程 (MQTT 订阅 -> BLE 写入)
    if (pthread_create(&downlink_tid, NULL, downlink_thread_func, NULL) != 0)
    {
        log_error("Main: Failed to create downlink thread.\n");
        pthread_cancel(uplink_tid);
        return -1;
    }
    log_debug("Main: Downlink thread created.\n");

    log_info("Main: Gateway application is running. Press Ctrl+C to exit.\n");

    while(keep_running)
    {
        sleep(1);
    }

    log_info("Main: Received exit signal, cleaning up resources...\n");

    pthread_cancel(uplink_tid);
    pthread_cancel(downlink_tid);

    pthread_join(uplink_tid, NULL);
    pthread_join(downlink_tid, NULL);

    log_info("Main: All threads have exited.\n");

    return 0;
}

