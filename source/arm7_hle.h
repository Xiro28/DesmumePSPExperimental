#pragma once
#include "types.h"
#include "FIFO.h"
#include "armcpu.h"
#include "registers.h"
#include "rtc.h"

extern u16 IPCSync9, IPCSync7;
extern u16 IPCFIFOCnt9, IPCFIFOCnt7;

void SendIPCReply(u32 service, u32 data, u32 flag = 0);

void OnIPCRequest();

void HLE_Reset();
void HLE_IPCSYNC();

void StartScanline(u32 line);

void executeARM7Stuff();