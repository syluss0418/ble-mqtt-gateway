/**********************************************************************
 *   Copyright: (C)2025 LingYun IoT System Studio
 *      Author: LiJiahui<2199250859@qq.com>
 *
 * Description: The purpose of this code is to provide a simple C library,
 *              which providing software bit-bang of the I2C protocol on
 *              any GPIO pins for ISKBoard.
 *
 *   ChangeLog:
 *        Version    Date       Author            Description
 *        V1.0.0  2024.08.29    LiJiahui      Release initial version
 *
 ***********************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "isl1208.h"


#define ISL1208_RTC_SECTION_LEN	7 /* RTC时间部分的寄存器总数(7个字节) */
#define ISL1208_REG_SC			0x00
#define ISL1208_REG_MN			0x01
#define ISL1208_REG_HR			0x02
#define ISL1208_REG_HR_MIL		(1<<7)
#define ISL1208_REG_HR_PM		(1<<5)
#define ISL1208_REG_DT			0x03
#define ISL1208_REG_MO			0x04
#define ISL1208_REG_YR			0x05
#define ISL1208_REG_DW			0x06
#define REGS_RTC_SR_LEN			8

#define ISL1208_REG_SR			0x07 /* 状态寄存器 */
#define ISL1208_REG_SR_WRTC		(1<<4)
#define ISL1208_REG_SR_RTCF		(1<<0)


const char *weekday[7] = {"Sunday.", "Monday.", "Tuesday.", "Wednesday.", "Thursday.", "Friday.", "Saturday."};

/* 调试打印（可选）*/
#define CONFIG_RTC_DEBUG

#ifdef	CONFIG_RTC_DEBUF
#define rtc_print(format, args...) printf(format, ##args)
#else
#define rtc_print(format, args...) do{} while(0)
#endif

/* __attribute__(unused):显示标记函数可能未被调用（避免编译器警告） */
#ifdef CONFIG_RTC_DEBUG
__attribute__((unused)) static void dump_buf(const char *prompt, uint8_t *buf, uint32_t size)
{
	int	i;

	if(!buf)
		return ;

	if(prompt)
		printf("%s\r\n", prompt);

	for(i=0; i<size; i++)
		printf("%02x ", buf[i]);

	printf("\r\n");
}
#endif



/*
 *+----------------------------------+
 *|     ISL1208 Low level API        |
 *+----------------------------------+
 */

/* 通过I2C总线从ISL1208芯片读取寄存器数据
 * 从ISL1208的regaddr寄存器开始，连续读取len个寄存器内容，结果存放到regs[]中 */
static int isl1208_i2c_read_regs(uint8_t regaddr, uint8_t *regs, uint8_t len)
{
	uint8_t i;
	int		rv = 0;
	uint8_t byte;
	uint8_t	ack;

	if(!regs || len<=0)
	{
		rtc_print("ISL1208:Invalid input arguments\r\n");
		return -PARM_ERROR;
	}

	//发送I2C起始条件
	I2C_StartCondition(ISL1208_I2CBUS);

	//发送芯片地址（写模式），告诉芯片要写入寄存器地址
	if(ERROR_NONE != (rv = I2C_SendAddress(ISL1208_I2CBUS, I2C_WR)))
	{
		rtc_print("ISL1208:Send chipest address[W] failre:rv=0x%02x\r\n", rv);
		rv = -rv;
		goto OUT;
	}

	//向芯片发送要读取的寄存器起始地址
	if(ERROR_NONE != (rv = I2C_WriteByte(ISL1208_I2CBUS, regaddr)))
	{
		rtc_print("ISL1208:Set register[0x%02x] failure:rc = 0x%02x\r\n", regaddr, rv);
		rv = -rv;
		goto OUT;
	}

	//重新发送起始条件（重启动）
	I2C_StartCondition(ISL1208_I2CBUS);

	//发送芯片地址（读模式），现在开始读取数据
	if(ERROR_NONE != (rv = I2C_SendAddress(ISL1208_I2CBUS, I2C_RD)))
	{
		rtc_print("ISL1208:Send chipest address[R] failure:rv = 0x%02x\r\n", rv);
		rv = -rv;
		goto OUT;
	}

	//读取连续指定数量的字节
	for(i=0; i<len; i++)
	{
		//最后一个字节不发送ACK，其他字节发送ACK
		if(i == (len - 1))
			ack = ACK_NONE; /* 最后一个字节，发送NACK */
		else
			ack = ACK; /* 不是最后一个字节，发送ACK */

		//读取一个字节
		rv = I2C_ReadByte(ISL1208_I2CBUS, &byte, ack, I2C_CLK_STRETCH_TIMEOUT);
		if(ERROR_NONE != rv)
		{
			rtc_print("ISL1208:Read register data failure:rv = 0x&02x\r\n", rv);
			rv = -rv;
			goto OUT;
		}

		regs[i] = byte; /* 将读取的字节存入缓冲区 */
	}

OUT:
	//发送I2C停止条件，结束通信
	I2C_StopCondition(ISL1208_I2CBUS);
	return rv;
}

/* 通过I2C总线向ISL1208芯片写入寄存器地址
 * 参数：regaddr:要写入的起始寄存器地址  regs：包含要写入数据的缓冲区指针  len：要写入的字节数
 *  */
static int isl1208_i2c_write_regs(uint8_t regaddr, uint8_t *regs, uint8_t len)
{
	uint8_t	i;
	int		rv = 0;

	//发送I2C起始条件
	I2C_StartCondition(ISL1208_I2CBUS);

	//发送芯片地址（写模式）
	if(ERROR_NONE != (rv = I2C_SendAddress(ISL1208_I2CBUS, I2C_WR)))
	{
		rtc_print("ISL1208:Send chipest address[W] failure:rv = 0x%02x\r\n", rv);
		rv = -rv;
		goto OUT;
	}

	//发送要写入的寄存器起始地址
	if(ERROR_NONE != (rv = I2C_WriteByte(ISL1208_I2CBUS, regaddr)))
	{
		rtc_print("ISL1208:Set register[0x%02x] failure :rv = 0x%02x\r\n", regaddr, rv);
		rv = -rv;
		goto OUT;
	}

	//连续写入指定数量的数据字节
	for(i=0; i<len; i++)
	{
		rv = I2C_WriteByte(ISL1208_I2CBUS, regs[i]);
		if(ERROR_NONE != rv)
		{
			rtc_print("ISL1208:Write regisater data failure:rv = 0x%02x\r\n", rv);
			rv = -rv;
			goto OUT;
		}
	}

	rv = 0; /* 成功完成 */

OUT:
	//发送I2C停止条件，结束通信
	I2C_StopCondition(ISL1208_I2CBUS);
	return rv;
}


/* ISL1208通常用BCD码存储时间, BCD码:每个字节的高四位和低四位分别表示十位和个位数字 */
#define bcd2bin(x)	(((x) & 0x0f) + ((x) >> 4) * 10) /* bcd转十进制：低四位+高四位*10 */
#define bin2bcd(x)	((((x) / 10) << 4) + (x) % 10)	 /* 十进制转bcd：十位数左移四位+个位数 */

/* 设置RTC芯片的当前时间，将tm中的时间信息写入ISL1208的RTC寄存器中 */
int set_rtc_time(rtc_time_t tm)
{
	int		rv;
	uint8_t	regs[ISL1208_RTC_SECTION_LEN] = {0, }; /* 0后面的，为尾随逗号，表示其余元素自动初始化为0 */
	uint8_t	sr;

	//将时间数据转换为BCD格式并存入寄存器缓冲区
	regs[ISL1208_REG_SC] = bin2bcd(tm.tm_sec);	//秒（0-59）
	regs[ISL1208_REG_MN] = bin2bcd(tm.tm_min);  //分（0-59）
	regs[ISL1208_REG_HR] = bin2bcd(tm.tm_hour) | ISL1208_REG_HR_MIL;  //时（设置为24小时制）

	regs[ISL1208_REG_DT] = bin2bcd(tm.tm_mday);  //日（1-31）
	regs[ISL1208_REG_MO] = bin2bcd(tm.tm_mon);   //月（1-12）
	regs[ISL1208_REG_YR] = bin2bcd(tm.tm_year - 2000); //年（减去2000得到00-99），减2000是因为：8位寄存器只能存0-99
	regs[ISL1208_REG_DW] = bin2bcd(tm.tm_wday & 7);	//星期（0-6），&7：保证结果范围在0~7

	//初始化I2C总线
	if(i2c_init(ISL1208_I2CBUS, ISL1208_CHIPADDR))
	{
		rtc_print("ISL1208:Initial I2C bus failure\r\n");
		return -1;
	}

	//读取状态寄存器，然后设置WRTC位允许写入时间
	rv = isl1208_i2c_read_regs(ISL1208_REG_SR, &sr, 1);
	if(rv < 0)
	{
		rtc_print("ISL1208:read Status Register failure,rv = %d\r\n", rv);
		rv = -2;
		goto OUT;
	}

	sr |= ISL1208_REG_SR_WRTC; /* 设置WRTC位（第四位）为1，允许写入RTC寄存器 */
	rv = isl1208_i2c_write_regs(ISL1208_REG_SR, &sr, 1);
	if(rv < 0)
	{
		rtc_print("ISL1208:Set Status Register WRTC failure,rv = %d\r\n", rv);
		rv = -3;
		goto OUT;
	}

	//写入时间数据到RTC寄存器（从秒寄存器开始，连续写入7个寄存器）
	rv = isl1208_i2c_write_regs(ISL1208_REG_SC, regs, ISL1208_RTC_SECTION_LEN);
	if(rv < 0)
	{
		rtc_print("ISL1208：Set RTC section registers failure,rv = %d\r\n", rv);
		rv = -4;
		goto OUT;
	}

	//清除WRTC位，禁止写入RTC寄存器（保护时间数据）
	sr &= (~ISL1208_REG_SR_WRTC); /* 将WRTC位清零 */
	rv = isl1208_i2c_write_regs(ISL1208_REG_SR, &sr, 1);
	if(rv < 0)
	{
		rtc_print("ISL1208:Clear status register WRTC failure,rv=%d\r\n", rv);
		rv = -5;
		goto OUT;
	}

OUT:
	//关闭I2C总线
	i2c_term(ISL1208_I2CBUS);
	return rv;
}


/* 从RTC芯片读取当前时间 */
int get_rtc_time(rtc_time_t *tm)
{
	int	rv = 0;
	uint8_t	regs[REGS_RTC_SR_LEN] = {0, };

	//检查输入参数的有效性
	if(!tm)
	{
		rtc_print("ISL1208:Invalid input arguments\r\n");
		return -1;
	}

	//初始化I2C总线
	if(i2c_init(ISL1208_I2CBUS, ISL1208_CHIPADDR))
	{
		rtc_print("ISL1208:Initial I2C bus failure\r\n");
		return -2;
	}

	//从秒寄存器开始，连续读取八个寄存器（7个时间寄存器+1个状态寄存器）
	rv = isl1208_i2c_read_regs(ISL1208_REG_SC, regs, REGS_RTC_SR_LEN);
	if(rv < 0)
	{
		rtc_print("ISL1208:read RTC_SECTION and SR registers failure,rv=%d\r\n", rv);
		rv = -3;
		goto OUT;
	}

	//检查RTC失效标志位，判断时间是否可靠
	if(regs[ISL1208_REG_SR] &ISL1208_REG_SR_RTCF)
	{
		rtc_print("ISL1208:Initialize RTC time after power failure\r\n");
		rv = -4; /* 时间不可靠，需要重新设置 */
		goto OUT;
	}

	//将BCD格式的寄存器数据转换为十进制并存入时间结构体
	tm->tm_sec = bcd2bin(regs[ISL1208_REG_SC]); //秒
	tm->tm_min = bcd2bin(regs[ISL1208_REG_MN]); //分

	//小时寄存器需要特殊处理，因为支持12小时和24小时两种格式
	{
		const uint8_t	_hr = regs[ISL1208_REG_HR];

		if(_hr & ISL1208_REG_HR_MIL) //检查是否为24小时制(MIL位=1)
		{
			//24小时制：直接转换低六位（0-23）
			tm->tm_hour = bcd2bin(_hr & 0x3f);
		}
		else //12小时制(MIL位=0)
		{
			//12小时制:只需转换低5位（范围：1-12）
			tm->tm_hour = bcd2bin(_hr & 0x1f);
			if(_hr & ISL1208_REG_HR_PM) //如果是PM（第五位为1：表示下午）
				tm->tm_hour +=12; //加12小时转换为24小时制
		}
	}

	tm->tm_mday = bcd2bin(regs[ISL1208_REG_DT]);	//日
	tm->tm_mon = bcd2bin(regs[ISL1208_REG_MO]);		//月
	tm->tm_year = bcd2bin(regs[ISL1208_REG_YR]) + 2000; //年（加2000得到完整年份）
	tm->tm_wday = bcd2bin(regs[ISL1208_REG_DW]);  	//星期

OUT:
	//关闭I2C总线
	i2c_term(ISL1208_I2CBUS);
	return rv;
}

//读取并打印当前RTC时间
void print_rtc_time(void)
{
	rtc_time_t tm;

	//读取当前时间
	if(get_rtc_time(&tm) < 0)
		return ;

	//打印
	printf("%04d-%02d-%02d %02d:%02d:%02d %s\r\n",
			tm.tm_year, tm.tm_mon, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec,
			weekday[tm.tm_wday]);
}

