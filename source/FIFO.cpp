/*
	Copyright 2006 yopyop
	Copyright 2007 shash
	Copyright 2007-2015 DeSmuME team

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

#include "FIFO.h"

#include <string.h>

#include "armcpu.h"
#include "debug.h"
#include "mem.h"
#include "MMU.h"
#include "registers.h"
#include "NDSSystem.h"
#include "gfx3d.h"

#include "arm7_hle.h"


// ========================================================= IPC FIFO
FIFO<u32, 16> IPCFIFO9; // FIFO in which the ARM9 writes
FIFO<u32, 16> IPCFIFO7;

void IPC_FIFOinit(u8 proc)
{
	IPCFIFO9.Clear();
    IPCFIFO7.Clear();
}

void IPC_FIFOsend(u8 proc, u32 val)
{

	if (IPCFIFOCnt9 & IPCFIFOCNT_FIFOENABLE){

		if (IPCFIFO9.IsFull()){
			IPCFIFOCnt9 |= 0x4000;
			return;
		}else
		{
			bool wasempty = IPCFIFO9.IsEmpty();
			IPCFIFO9.Write(val);
			OnIPCRequest();
			if ((IPCFIFOCnt7 & 0x0400) && wasempty)
				NDS_makeIrq(1, IRQ_BIT_IPCFIFO_RECVNONEMPTY);
		}
		
		NDS_Reschedule();
	}
}

u32 IPC_FIFOrecv(u8 proc)
{
	 if (IPCFIFOCnt9 & 0x8000)
        {
            u32 ret;
            if (IPCFIFO7.IsEmpty())
            {
                IPCFIFOCnt9 |= 0x4000;
                return 0;
            }
            else
            {
                ret = IPCFIFO7.Read();

                if (IPCFIFO7.IsEmpty() && (IPCFIFOCnt7 & 0x0004))
                    NDS_makeIrq(1, IRQ_BIT_IPCFIFO_SENDEMPTY);
				
				NDS_Reschedule();
            }

            return ret;
        }
        else
            return 0;
}

void IPC_FIFOcnt(u8 proc, u16 val)
{
	if (val & 0x0008)
		IPCFIFO9.Clear();
	if ((val & 0x0004) && (!(IPCFIFOCnt9 & 0x0004)) && IPCFIFO9.IsEmpty())
		NDS_makeIrq(0, IRQ_BIT_IPCFIFO_SENDEMPTY);
	if ((val & 0x0400) && (!(IPCFIFOCnt9 & 0x0400)) && (!IPCFIFO7.IsEmpty()))
		NDS_makeIrq(0, IRQ_BIT_IPCFIFO_RECVNONEMPTY);
	if (val & 0x4000)
		IPCFIFOCnt9 &= ~0x4000;
	IPCFIFOCnt9 = (val & 0x8404) | (IPCFIFOCnt9 & 0x4000);

	NDS_Reschedule();
}

u32 IPC_FIFOgetCnt(u8 proc)
{
	u16 val = IPCFIFOCnt9;
	if (IPCFIFO9.IsEmpty())     val |= 0x0001;
	else if (IPCFIFO9.IsFull()) val |= 0x0002;
	if (IPCFIFO7.IsEmpty())     val |= 0x0100;
	else if (IPCFIFO7.IsFull()) val |= 0x0200;
	return val;
}

// ========================================================= GFX FIFO
GFX_PIPE	gxPIPE;
GFX_FIFO	gxFIFO;

void GFX_PIPEclear()
{
	gxPIPE.head = 0;
	gxPIPE.tail = 0;
	gxPIPE.size = 0;
	gxFIFO.matrix_stack_op_size = 0;
}

void GFX_FIFOclear()
{
	gxFIFO.head = 0;
	gxFIFO.tail = 0;
	gxFIFO.size = 0;
	gxFIFO.matrix_stack_op_size = 0;
}

static void GXF_FIFO_handleEvents()
{
	bool low = gxFIFO.size <= 127;
	bool lowchange = MMU_new.gxstat.fifo_low ^ low;
	MMU_new.gxstat.fifo_low = low;
	if(low) triggerDma(EDMAMode_GXFifo);

	bool empty = gxFIFO.size == 0;
	bool emptychange = MMU_new.gxstat.fifo_empty ^ empty;
	MMU_new.gxstat.fifo_empty = empty;


	MMU_new.gxstat.sb = gxFIFO.matrix_stack_op_size != 0;

	if(emptychange||lowchange) NDS_Reschedule();
}

static bool IsMatrixStackCommand(u8 cmd)
{
	return cmd == 0x11 || cmd == 0x12;
}

void GFX_FIFOsend(u8 cmd, u32 param)
{
	//INFO("gxFIFO: send 0x%02X = 0x%08X (size %03i/0x%02X) gxstat 0x%08X\n", cmd, param, gxFIFO.size, gxFIFO.size, gxstat);
	//printf("fifo recv: %02X: %08X upto:%d\n",cmd,param,gxFIFO.size+1);

	//TODO - WOAH ! NOT HANDLING A TOO-BIG FIFO RIGHT NOW!
	//if (gxFIFO.size > 255)
	//{
	//	GXF_FIFO_handleEvents();
	//	//NEED TO HANDLE THIS!!!!!!!!!!!!!!!!!!!!!!!!!!

	//	//gxstat |= 0x08000000;			// busy
	//	NDS_RescheduleGXFIFO(1);
	//	//INFO("ERROR: gxFIFO is full (cmd 0x%02X = 0x%08X) (prev cmd 0x%02X = 0x%08X)\n", cmd, param, gxFIFO.cmd[255], gxFIFO.param[255]);
	//	return;		
	//}


	gxFIFO.cmd[gxFIFO.tail] = cmd;
	gxFIFO.param[gxFIFO.tail] = param;
	gxFIFO.tail++;
	gxFIFO.size++;
	if (gxFIFO.tail > HACK_GXIFO_SIZE-1) gxFIFO.tail = 0;

	//if a matrix op is entering the pipeline, do accounting for it
	//(this is tested by wild west, which will jam a few ops in the fifo and then wait for the matrix stack to be 
	//un-busy so it can read back the current matrix stack position).
	//it is definitely only pushes and pops which set this flag.
	//seems like it would be less work in the HW to make a counter than do cmps on all the command bytes, so maybe we're even doing it right.
	if(IsMatrixStackCommand(cmd))
		gxFIFO.matrix_stack_op_size++;

	/*
	if(gxFIFO.size>=HACK_GXIFO_SIZE) {
		printf("--FIFO FULL-- : %d\n",gxFIFO.size);
	}
	*/
	
	//gxstat |= 0x08000000;		// set busy flag

	GXF_FIFO_handleEvents();

	NDS_RescheduleGXFIFO(1); 
}

// this function used ONLY in gxFIFO
BOOL GFX_PIPErecv(u8 *cmd, u32 *param)
{
	//gxstat &= 0xF7FFFFFF;		// clear busy flag

	if (gxFIFO.size == 0)
	{
		GXF_FIFO_handleEvents();
		return FALSE;
	}

	*cmd = gxFIFO.cmd[gxFIFO.head];
	*param = gxFIFO.param[gxFIFO.head];

	//see the associated increment in another function
	if(IsMatrixStackCommand(*cmd))
	{
		gxFIFO.matrix_stack_op_size--;
		/**
		if(gxFIFO.matrix_stack_op_size>0x10000000)
			printf("bad news disaster in matrix_stack_op_size\n");
		**/
	}

	gxFIFO.head++;
	gxFIFO.size--;
	if (gxFIFO.head > HACK_GXIFO_SIZE-1) gxFIFO.head = 0;

	GXF_FIFO_handleEvents();

	return (TRUE);
}

void GFX_FIFOcnt(u32 val)
{
	////INFO("gxFIFO: write cnt 0x%08X (prev 0x%08X) FIFO size %03i PIPE size %03i\n", val, gxstat, gxFIFO.size, gxPIPE.size);

	if (val & (1<<29))		// clear? (only in homebrew?)
	{
		GFX_PIPEclear();
		GFX_FIFOclear();
		return;
	}

	//zeromus says: what happened to clear stack?
	//if (val & (1<<15))		// projection stack pointer reset
	//{
	//	gfx3d_ClearStack();
	//	val &= 0xFFFF5FFF;		// clear reset (bit15) & stack level (bit13)
	//}

	T1WriteLong(MMU.MMU_MEM[ARMCPU_ARM9][0x40], 0x600, val);
}

// ========================================================= DISP FIFO
DISP_FIFO	disp_fifo;

void DISP_FIFOinit()
{
	memset(&disp_fifo, 0, sizeof(DISP_FIFO));
}

void DISP_FIFOsend(u32 val)
{
	//INFO("DISP_FIFO send value 0x%08X (head 0x%06X, tail 0x%06X)\n", val, disp_fifo.head, disp_fifo.tail);
	disp_fifo.buf[disp_fifo.tail] = val;
	disp_fifo.tail++;
	if (disp_fifo.tail > 0x5FFF)
		disp_fifo.tail = 0;
}

u32 DISP_FIFOrecv()
{
	//if (disp_fifo.tail == disp_fifo.head) return (0); // FIFO is empty
	u32 val = disp_fifo.buf[disp_fifo.head];
	disp_fifo.head++;
	if (disp_fifo.head > 0x5FFF)
		disp_fifo.head = 0;
	return (val);
}
