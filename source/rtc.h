/*
	Copyright 2006 yopyop
	Copyright 2008 CrazyMax
	Copyright 2008-2010 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _RTC_H_
#define _RTC_H_
#include <stdlib.h>
#include <time.h>
#include "types.h"
//#include "utils/datetime.h"

//DateTime rtcGetTime(void);

typedef struct
{
	// RTC registers
	u8	regStatus1;
	u8	regStatus2;
	u8	regAdjustment;
	u8	regFree;

	// BUS
	u8	_prevSCK;
	u8	_prevCS;
	u8	_prevSIO;
	u8	_SCK;
	u8	_CS;
	u8	_SIO;
	u8	_DD;
	u16	_REG;

	// command & data
	u8	cmd;
	u8	cmdStat;
	u8	bitsCount;
	u8	data[8];

	u8 cmdBitsSize[8];
} _RTC;

extern _RTC	rtc;

extern void rtcRecv();

extern	void rtcInit();
extern	u16 rtcRead();
extern	void rtcWrite(u16 val);
#endif
