#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <dbus/dbus.h>


#define BLUEZ_BUS_NAME	"org.bluez"
#define ADAPTER_PATH	"/org/bluez/hci0"

#define DEVICE_MAC		"A0_76_4E_57_E9_62"
#define DEVICE_PATH     "/org/bluez/hci0/dev_" DEVICE_MAC

// 用于启用通知的特性对象路径（单片机-》树莓派）
#define NOTIFY_CHARACTERISTIC_PATH		DEVICE_PATH "/service0028/char0037"
//用于写入数据的特性对象路径（树莓派-》单片机）
#define WRITABLE_CHARACTERISTIC_PATH    DEVICE_PATH "/service0028/char002f"

int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method);
void print_notify_value(DBusMessageIter *variant_iter);
void handle_properties_changed(DBusMessage *msg);
int write_characteristic_value(DBusConnection *conn, const char* char_path, const char* cmd_str);


DBusConnection *conn = NULL;

int main(int argc, char **argv)
{
	DBusError	err; //用来接收D-BUS操作的错误状态
	int			write_counter = 0;


	//连接到D-BUS 系统总线
	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err))
	{
		fprintf(stderr, "D-BUS Connection Erroe: %s\n", err.message);
		dbus_error_free(&err);
		return -1;
	}
	printf("Connected to D-BUS system bus.\n");
	

	//建立蓝牙通信连接（连接到ESP32）
	printf("Connecting to BLE device %s...\n", DEVICE_MAC);
	//调用org.bluez.Device1接口中的Connect 方法
	if (call_method(conn, DEVICE_PATH, "org.bluez.Device1", "Connect") < 0)
	{
		fprintf(stderr, "Failed to connect to BLE device.\n");
		return -2;
	}
	printf("Successfully connected to BLE devices.\n");


	//启用特征值的通知功能
	printf("Enabling notifications for characteristic %s...\n", NOTIFY_CHARACTERISTIC_PATH);
	//调用org.bluez.GattCharacteristic1 接口中的StartNotify 方法
	if (call_method(conn, NOTIFY_CHARACTERISTIC_PATH, "org.bluez.GattCharacteristic1", "StartNotify") < 0)
	{
		fprintf(stderr, "Failed to enable notification.\n");
		return -3;
	}
	printf("Notification enabled.\n");


	//注册一个D-BUS 信号监听器，用于监听BLE 设备发来的通知数据
	//监听PropertiesChanged 信号，当特征值改变时会触发
	    dbus_bus_add_match(conn,"type='signal',""interface='org.freedesktop.DBus.Properties',""member='PropertiesChanged'", &err);
	
	if (dbus_error_is_set(&err))
	{
		fprintf(stderr, "D-BUS Match Rule Error: %ss\n", err.message);
		dbus_error_free(&err);
		return -4;
	}
	dbus_connection_flush(conn); //强制立即将D-BUS 链接上的所有待发送数据（包括匹配规则）推送到D-BUS 总线守护进程，避免错过信号
	printf("D-BUS signal match rule added for notification.\n");

	printf("\n--- Entering main communication loop---\n");
	printf("Listening for notifications and sending periodic writed...\n");

	
	while (true)
	{
		//监听BLE设备通知（Notify）
		//检查是否有可读的消息，并把收到的D-BUS消息加载到消息队列中
		dbus_connection_read_write(conn, 100);

		//从消息队列中弹出一条完整的消息
		DBusMessage *msg = dbus_connection_pop_message(conn);
	
		if (msg)
		{
			//判断是否是BLE的通知信号（PropertiseChanged）
			//并且判断这个信号是否来自我们关系的CHARACTERISTIC_PATH
			if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged") && strstr(dbus_message_get_path(msg), NOTIFY_CHARACTERISTIC_PATH))
			{
				handle_properties_changed(msg); //处理通知数据
			}
			dbus_message_unref(msg); //释放消息引用计数
		}


		//向连接设备发送数据（interval = 5s）
		if (write_counter %50 == 0)
		{
			//char send_data[32];
			//snprintf(send_data, sizeof(send_data), "PI %d!", write_counter / 50);
			char send_data[] = "A";
			printf("\nAttemping to write data :\"%s\" to %s\n", send_data, WRITABLE_CHARACTERISTIC_PATH);
			if (write_characteristic_value(conn, WRITABLE_CHARACTERISTIC_PATH, send_data) < 0)
			{
				fprintf(stderr, "Failed to write data to characteristic,\n");
			}
		}
		write_counter++;

		usleep(100000);
	}


	//clean
	if (conn)
	{
		dbus_connection_unref(conn);
	}
	printf("Progeam exited.\n");

	return 0;



}



/* 调用 D-BUS 上的一个方法
 * 参数1：D-BUS 连接对象	参数2：目标 D-BUS 对象路径
 * 参数3：目标D-BUS 接口名	参数4：目标方法名
 * return 0 成功，-1 创建消息失败， -2 方法调用失败
 */
int call_method(DBusConnection *conn, const char *path, const char *interface, const char *method)
{
	DBusMessage	*msg, *reply;
	DBusError	err;

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



/* 处理PropertiesChanged 信号，提取并打印特性值 */
void handle_properties_changed(DBusMessage *msg)
{
	DBusMessageIter		args;
	const char			*iface;
	DBusMessageIter		changed_props;
	DBusMessageIter		entry;
	const char			*key;
	DBusMessageIter		variant_iter;


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
			printf("-----------------------------------\n");
		}
		dbus_message_iter_next(&changed_props); //移动到下一个字典项
	}
}	



/* 打印接收到的通知值*/
void print_notify_value(DBusMessageIter *variant_iter)
{
	DBusMessageIter	array_iter;
	dbus_message_iter_recurse(variant_iter, &array_iter);

	printf("Received NOtification Value: ");
	while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID)
	{
		uint8_t byte_val;
		dbus_message_iter_get_basic(&array_iter, &byte_val);
		printf("%02x ", byte_val);
		dbus_message_iter_next(&array_iter);
	}
	printf(" (Hex)\n)");


	//转换为字符串打印
	printf("Decoded string: \"");
	dbus_message_iter_recurse(variant_iter, &array_iter); //重新初始化迭代器
	while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID)
	{
		uint8_t	byte_val;
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
	printf("\n");
}



/* 向BLE特征值写入数据 */
int write_characteristic_value(DBusConnection *conn, const char *char_path, const char *cmd_str)
{	
	DBusMessage		*msg;
	DBusMessageIter	args, array_iter, options_iter;
	DBusError		err;

	dbus_error_init(&err);

	//创建方法调用消息：org.bluez.GattCharacteristic1.WriteValue
	msg = dbus_message_new_method_call(BLUEZ_BUS_NAME, char_path, "org.bluez.GattCharacteristic1", "WriteValue");
	if (!msg)
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
    // 这里我们传递一个空字典，表示没有特殊选项
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


