#ifndef __PIDFILE_H
#define __PIDFILE_H

#define PID_FILE_NAME ".iot_gateway.pid"

// 错误码
#define PIDFILE_SUCCESS         0
#define PIDFILE_EXISTS_ERROR    -1 // PID文件已存在

int create_pid_file(const char *pid_file);
void remove_pid_file(const char *pid_file);

#endif //__PIDFILE_H

