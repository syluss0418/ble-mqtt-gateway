/*********************************************************************************
 *      Copyright:  (C) 2025 lijiahui<2199250859@qq.com>
 *                  All rights reserved.
 *
 *       Filename:  log.c
 *    Description:  This file 
 *                 
 *        Version:  1.0.0(25/07/30)
 *         Author:  lijiahui <2199250859@qq.com>
 *      ChangeLog:  1, Release initial version on "25/07/30 8:15:24"
 *                 
 ********************************************************************************/


#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <pthread.h>

#include <log.h>


//函数指针，用于日志系统的加锁和解锁
typedef void (*log_LockFn)(void *udata, int lock);


static struct {
	char		file[32]; //logger file name
	FILE		*fp;	  //logger file pointer
	long		size;     //logger file max size
	int			level;    //logger level
	log_LockFn	lockfn;   //lock function,线程锁函数指针
	void		*udata;   //lock data
} L;


static const char *level_names[] = {
	"ERROR",
	"WARN" ,
	"INFO" ,
	"DEBUG",
	"TRACE"
};


static const char *level_colors[] = {
	"\x1b[31m", //red
	"\x1b[33m", //yellow
	"\x1b[32m", //green
	"\x1b[36m", //cyan-blue
	"\x1b[94m"  //blue
};


//将当前时间格式化未字符串
static inline void time_to_str(char *buf)
{
	struct timeval	tv;
	struct tm		*tm;
	int				len;

	gettimeofday(&tv, NULL); //获取当前时间，精度为微妙
	tm = localtime(&tv.tv_sec); //将秒数转换为本地时间

	len = sprintf(buf, "%04d-%02d-%02d %2d:%02d:%02d.%06d", 
			tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
			tm->tm_hour, tm->tm_min, tm->tm_sec, (int)tv.tv_usec);
	buf[len] = '\0';
}


//互斥锁，用于加锁和解锁
//保护日志文件的读写操作，防止多个线程同时写入，避免数据混乱和文件损坏
static void mutex_lock(void *udata, int lock)
{
	int	err;
	pthread_mutex_t *l = (pthread_mutex_t *)udata;

	if(lock) //lock为1时加锁
	{
		if((err = pthread_mutex_lock(l)) != 0)
			log_error("Unable to lock log lock: %s\n", strerror(err));
	}
	else //解锁
	{ 
		if((err = pthread_mutex_unlock(l)) != 0)
			log_error("Unable to unlock log lock: %s\n", strerror(err));
	}
}



int log_open(char *fname, int level, int size, int lock)
{
	FILE	*fp;

	L.level = level; //设置日志级别
	L.size = size*1024; //设置日志文件最大大小（转换为字节）

	//判断日志输出控制台/文件
	if(!fname || strcmp(fname, "console") || strcmp(fname, "stderr"))
	{
		strcpy(L.file, "console");
		L.fp = stderr;
		L.size = 0; //因为控制台没有文件大小限制，此处不需要执行文件回滚
	}
	else //输出到文件
	{
		//追加+读写模式
		if(!(fp = fopen(fname, "a+")))
		{
			fprintf(stderr, "%s() failed: %s\n", __func__, strerror(errno));
			return -2;
		}
		L.fp = fp;
		strncpy(L.file, fname, sizeof(L.file)); //复制文件名
	}


	if(lock) //如果需要加锁
	{
		static pthread_mutex_t	log_lock;

		pthread_mutex_init(&log_lock, NULL);
		L.udata = (void *)&log_lock; //设置用户数据为互斥锁指针
		L.lockfn = mutex_lock; //设置锁函数
	}

	fprintf(L.fp, "\n");
	log_info("Logger system(%s) start: file:\"%s\", level: %s, maxsize: %lu KiB \n\n", 
			LOG_VERSION, L.file, level_names[level], size);

	return 0;
}



//close log system
void log_close(void)
{
	//如果文件指针存在且不是stderr， 关闭文件
	if(L.fp && L.fp != stderr)
		fclose(L.fp);

	//如果锁数据存在，销毁互斥锁
	if(L.udata)
		pthread_mutex_destroy(L.udata);
}



//日志文件回滚函数，当文件达到最大大小时，进行备份和清空
//当前日志文件会备份成一个名为.bak的文件
static void log_rollback(void)
{
	char	cmd[128] = {0};
	long	fsize;


	//如果最大大小为0或小于0，则不需要回滚
	if(L.size <= 0)
		return ;

	fsize = ftell(L.fp); //获取文件大小
	if(fsize < L.size) //如果文件大小为达到最大值，则不回滚
		return ;

	//备份当前日志文件
	snprintf(cmd, sizeof(cmd), "cp %s %s.bak", L.file, L.file);
	system(cmd); //执行操作系统的shell命令

	//回滚文件
	fseek(L.fp, 0, SEEK_SET); //将文件指针移动到文件开头
	truncate(L.file, 0); //将文件大小截断为0，清空文件内容

	fprintf(L.fp, "\n");
	log_info("Logger system(%s) rollback: flie: \"%s\", level:%s, maxsize: %lu KiB\n\n",
			LOG_VERSION, L.file, level_names[L.level], L.size/1024);

	return ;
}



//日志写入
void _log_write(int level, const char *file, int line, const char *fmt, ...)
{
	va_list	args; //处理可变参数（可类比指针和迭代器）
	char	time_string[100];

	if(!L.fp || level > L.level)
		return ;


	//Acquire lock
	if(L.lockfn)
		L.lockfn(L.udata, 1);


	//check and rollback file
	log_rollback();

	time_to_str(time_string);

	//LOg to stderr
	if(L.fp == stderr)
	{
		fprintf(L.fp, "%s %s %-5s\x1b[0m \x1b[90m%s:%03d:\x1b[0m ",
				time_string, level_colors[level], level_names[level], file, line);
	}
	else //log to file
	{
		fprintf(L.fp, "%s %-5s %s:%03d: ", time_string, level_names[level], file, line);
	}

	va_start(args, fmt); //初始化可变参数列表
	vfprintf(L.fp, fmt, args); //写入可变参数的日志信息
	va_end(args); //结束可变参数列表

	fflush(L.fp);

	//release lock
	if(L.lockfn)
		L.lockfn(L.udata, 0);
}


//日志数据转储函数
void log_dump(int level, const char *prompt, char *buf, size_t len)
{
	int				i, j, ofset;
	char			line[256];
	unsigned char	c;
	unsigned char	*buffer = (unsigned char *)buf;

	if(!L.fp || level > L.level)
		return ;


	if(prompt)
		_log_write(level, __FILE__, __LINE__, "%s\r\n", prompt);


	for(i = 0; i < len; i += 16)
	{
		//格式化行首偏移量
		ofset = snprintf(line, sizeof(line), "%04x: ", i);

		//打印十六进制，如果超出缓冲区则打印空格
		for(j = 0; j< 16; j++)
		{
			if(i+j < len) //打印一个字节的十六进制
				ofset += snprintf(line+ofset, sizeof(line)-ofset, "%02x ", buffer[i+j]);
			else //不足16个字符，用空格填充
				ofset += snprintf(line+ofset, sizeof(line)-ofset, "   ");

		}
		ofset += snprintf(line+ofset, sizeof(line)-ofset, " ");

		//打印ASCII 表示
		for(j = 0; j < 16; j++)
		{
			if(i+j < len)
			{
				c = buffer[i+j];
				//判断是否是可打印字符，是则打印，否则打印“.”
				ofset += snprintf(line+ofset, sizeof(line)-ofset, "%c", (c>32 && c<126) ? c : '.');
			}
			else //if不足16个字符，用空格填充
			{
				ofset += snprintf(line+ofset, sizeof(line)-ofset, " ");
			}
		}

		if(L.fp)
			fprintf(L.fp, "%s\r\n", line); //打印整行到文件
	}
}




