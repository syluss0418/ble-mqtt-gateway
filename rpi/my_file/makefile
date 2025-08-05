# 定义编译器
CC = gcc

# 定义编译选项
# -I: 告诉编译器查找头文件的目录
# -Wall: 开启所有警告
# -g: 生成调试信息
# 增加了 D-Bus 和 Mosquitto 的头文件路径
CFLAGS = -I./lib -Wall -g \
         -I/usr/include/dbus-1.0 \
         -I/usr/lib/arm-linux-gnueabihf/dbus-1.0/include \
         -I/usr/include/mosquitto \
         -I/usr/include/json-c # json-c 的头文件路径也最好在这里明确指定

# 定义链接库
# -l: 链接库
# 移除了 LDLIBS 中错误的 -I 参数，只保留 -l 参数
LDLIBS = -lmosquitto -ldbus-1 -ljson-c -lpthread # 保持正确的链接顺序和库名

# 定义源文件和目标文件
SRCS = main.c src/ble_gateway.c src/mqtt_gateway.c src/log.c src/config_parser.c
OBJS = $(SRCS:.c=.o) # 将所有.c文件转换为.o文件

# 定义可执行文件名称
TARGET = iot_gateway

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDLIBS) -o $@

# 编译规则：如何从.c文件生成.o文件
# $<: 表示第一个依赖项 (即.c文件)
# $@: 表示目标文件 (即.o文件)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
