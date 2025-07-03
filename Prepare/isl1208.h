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

#ifndef RTC_ISL1208_H_
#define RTC_ISL1208_H_

#include "i2c_bitbang.h"

#define ISL1208_I2CBUS		I2CBUS0 /* ISL1208 on GPIO I2C bus0 */
#define ISL1208_CHIPADDR	0x6f 	/* ISL1208 7-Bits Chip address(from datasheet) */

typedef struct rtc_time_s
{
	int	tm_sec;	 /* 秒（0~59）*/
	int	tm_min;  /* 分（0~59）*/
	int	tm_hour; /* 时（0~23）*/

	int	tm_mday; /* 日（1~31）*/
	int	tm_mon;  /* 月（1~12）*/
	int	tm_year; /* 年（2000~2099）*/

	int	tm_wday; /* 星期（0~6）*/
}rtc_time_t;

extern const char *weekday[7];

extern int set_rtc_time(rtc_time_t tm);

extern int get_rtc_time(rtc_time_t *tm);

extern void print_rtc_time(void);

#endif;
