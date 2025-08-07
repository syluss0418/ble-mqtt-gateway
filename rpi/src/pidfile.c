#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "log.h"
#include "pidfile.h"


/* 检查并创建一个PID文件，确保只有一个实例在运行
 * pid_file: PID文件的路径
 */
int create_pid_file(const char *pid_file)
{
    int             pidfd;
    char            pid_buf[16];
    
    // 尝试创建并打开PID文件，使用独占模式，如果文件已存在则失败
    pidfd = open(pid_file, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if(pidfd < 0)
    {
        // 如果文件已存在
        if (errno == EEXIST)
        {
            return PIDFILE_EXISTS_ERROR;
        }
        return -2;
    }

    // 加锁
    if(lockf(pidfd, F_TLOCK, 0) < 0)
    {
        close(pidfd);
        unlink(pid_file); 
        return -3;
    }


    // 清空文件内容
    if(ftruncate(pidfd, 0) < 0)
    {
        close(pidfd);
		unlink(pid_file);
        return -4;
    }


    // 写入当前进程的PID
    snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
    if(write(pidfd, pid_buf, strlen(pid_buf)) < 0)
    {
        close(pidfd);
		unlink(pid_file);
        return -5;
    }

    close(pidfd);

    return PIDFILE_SUCCESS;
}


/* 删除PID文件 */
void remove_pid_file(const char *pid_file)
{
    if(pid_file && access(pid_file, F_OK) == 0)
    {
        log_info("Removing PID file %s...\n", pid_file);
        unlink(pid_file);
    }
}

