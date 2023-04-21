/*	Copyright (C) 2006 yopyop
	Copyright (C) 2011 Loren Merritt
	Copyright (C) 2012 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "types.h"

#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>


#include "MMU.h"
#include "MMU_timing.h"

#include "bios.h"

#include "PSP/FrontEnd.h"
#include "blockdecoder.h"

#include "PSP/emit/psp_emit.h"
#include "mips_code_emiter.h"

#include "Disassembler.h"

#include <pspsuspend.h>

u32 saveBlockSizeJIT = 0;
uint32_t interpreted_cycles = 0;
uint32_t base_adr = 0;

CACHE_ALIGN JIT_struct JIT;

uintptr_t *JIT_struct::JIT_MEM[2][0x4000] = {{0}};

//static u8 recompile_counts[(1<<26)/16];

static uintptr_t *JIT_MEM[2][32] = {
   //arm9
   {
      /* 0X*/  DUP2(JIT.ARM9_ITCM),
      /* 1X*/  DUP2(JIT.ARM9_ITCM), // mirror
      /* 2X*/  DUP2(JIT.MAIN_MEM),
      /* 3X*/  DUP2(JIT.SWIRAM),
      /* 4X*/  DUP2(NULL),
      /* 5X*/  DUP2(NULL),
      /* 6X*/      NULL, 
                JIT.ARM9_LCDC,   // Plain ARM9-CPU Access (LCDC mode) (max 656KB)
      /* 7X*/  DUP2(NULL),
      /* 8X*/  DUP2(NULL),
      /* 9X*/  DUP2(NULL),
      /* AX*/  DUP2(NULL),
      /* BX*/  DUP2(NULL),
      /* CX*/  DUP2(NULL),
      /* DX*/  DUP2(NULL),
      /* EX*/  DUP2(NULL),
      /* FX*/  DUP2(JIT.ARM9_BIOS)
   },
   //arm7
   {
      /* 0X*/  DUP2(JIT.ARM7_BIOS),
      /* 1X*/  DUP2(NULL),
      /* 2X*/  DUP2(JIT.MAIN_MEM),
      /* 3X*/       JIT.SWIRAM,
                   JIT.ARM7_ERAM,
      /* 4X*/       NULL,
                   JIT.ARM7_WIRAM,
      /* 5X*/  DUP2(NULL),
      /* 6X*/      JIT.ARM7_WRAM,      // VRAM allocated as Work RAM to ARM7 (max. 256K)
                NULL,
      /* 7X*/  DUP2(NULL),
      /* 8X*/  DUP2(NULL),
      /* 9X*/  DUP2(NULL),
      /* AX*/  DUP2(NULL),
      /* BX*/  DUP2(NULL),
      /* CX*/  DUP2(NULL),
      /* DX*/  DUP2(NULL),
      /* EX*/  DUP2(NULL),
      /* FX*/  DUP2(NULL)
      }
};

static u32 JIT_MASK[2][32] = {
   //arm9
   {
      /* 0X*/  DUP2(0x00007FFF),
      /* 1X*/  DUP2(0x00007FFF),
      /* 2X*/  DUP2(0x003FFFFF), // FIXME _MMU_MAIN_MEM_MASK
      /* 3X*/  DUP2(0x00007FFF),
      /* 4X*/  DUP2(0x00000000),
      /* 5X*/  DUP2(0x00000000),
      /* 6X*/      0x00000000,
                0x000FFFFF,
      /* 7X*/  DUP2(0x00000000),
      /* 8X*/  DUP2(0x00000000),
      /* 9X*/  DUP2(0x00000000),
      /* AX*/  DUP2(0x00000000),
      /* BX*/  DUP2(0x00000000),
      /* CX*/  DUP2(0x00000000),
      /* DX*/  DUP2(0x00000000),
      /* EX*/  DUP2(0x00000000),
      /* FX*/  DUP2(0x00007FFF)
   },
   //arm7
   {
      /* 0X*/  DUP2(0x00003FFF),
      /* 1X*/  DUP2(0x00000000),
      /* 2X*/  DUP2(0x003FFFFF),
      /* 3X*/       0x00007FFF,
                   0x0000FFFF,
      /* 4X*/       0x00000000,
                   0x0000FFFF,
      /* 5X*/  DUP2(0x00000000),
      /* 6X*/      0x0003FFFF,
                0x00000000,
      /* 7X*/  DUP2(0x00000000),
      /* 8X*/  DUP2(0x00000000),
      /* 9X*/  DUP2(0x00000000),
      /* AX*/  DUP2(0x00000000),
      /* BX*/  DUP2(0x00000000),
      /* CX*/  DUP2(0x00000000),
      /* DX*/  DUP2(0x00000000),
      /* EX*/  DUP2(0x00000000),
      /* FX*/  DUP2(0x00000000)
      }
};

static void init_jit_mem()
{
   static bool inited = false;  
   if(inited)
      return;
   inited = true;
   for(int proc=0; proc<2; proc++)
      for(int i=0; i<0x4000; i++)
         JIT.JIT_MEM[proc][i] = JIT_MEM[proc][i>>9] + (((i<<14) & JIT_MASK[proc][i>>9]) >> 1);
   
   currentBlock.init(); 
} 

static bool thumb = false;
static uint32_t pc = 0;

static u32 instr_attributes(u32 opcode)
{
   return thumb ? thumb_attributes[opcode>>6]
                : instruction_attributes[INSTRUCTION_INDEX(opcode)];
}

static bool instr_is_conditional(u32 opcode)
{
	if(thumb) return false;
	
	return !(CONDITION(opcode) == 0xE
	         || (CONDITION(opcode) == 0xF && CODE(opcode) == 5));
}

static bool instr_is_branch(u32 opcode)
{ 
   u32 x = instr_attributes(opcode);
   if(thumb)
      return (x & BRANCH_ALWAYS)
          || ((x & BRANCH_POS0) && ((opcode&7) | ((opcode>>4)&8)) == 15)
          || (x & BRANCH_SWI)
          || (x & JIT_BYPASS);
   else
      return (x & BRANCH_ALWAYS)
          || ((x & BRANCH_POS12) && REG_POS(opcode,12) == 15)
          || ((x & BRANCH_LDM) && BIT15(opcode))
          || (x & BRANCH_SWI)
          || (x & JIT_BYPASS);
}

enum INSTR_R { DYNAREC, INTERPRET };

typedef INSTR_R (*DynaCompiler)(uint32_t pc, uint32_t opcode);

#define disable_op(name, PREOP, ...) static INSTR_R ARM_OP_##name##_##PREOP (uint32_t pc, const u32 i) { return INTERPRET; }

#define ARM_OP_REG_(name, PREOP, _rd) \
   static INSTR_R ARM_OP_##name##_##PREOP (uint32_t pc, const u32 i)\
   {\
      currentBlock.addOP(OP_##name, pc, _rd, REG_POS(i,16), (REG_POS(i,8) << 8 | REG_POS(i, 0)), 0, PRE_OP_##PREOP, instr_is_conditional(i) ? CONDITION(i) : -1);\
      return DYNAREC; \
   }

#define ARM_OP_IMM_(name, PREOP, _rd) \
   static INSTR_R ARM_OP_##name##_##PREOP (uint32_t pc, const u32 i)\
   {\
      currentBlock.addOP(OP_##name, pc, _rd, REG_POS(i,16), REG_POS(i, 0), ((i>>7)&0x1F), PRE_OP_##PREOP, instr_is_conditional(i) ? CONDITION(i) : -1);\
      return DYNAREC; \
   }

#define ARM_OP_IMM(name, PREOP) ARM_OP_IMM_(name, PREOP, REG_POS(i,12))
#define ARM_OP_REG(name, PREOP) ARM_OP_REG_(name, PREOP, REG_POS(i,12))

#define ARM_OP_UNDEF(T) \
   static const DynaCompiler ARM_OP_##T##_LSL_IMM = 0; \
   static const DynaCompiler ARM_OP_##T##_LSL_REG = 0; \
   static const DynaCompiler ARM_OP_##T##_LSR_IMM = 0; \
   static const DynaCompiler ARM_OP_##T##_LSR_REG = 0; \
   static const DynaCompiler ARM_OP_##T##_ASR_IMM = 0; \
   static const DynaCompiler ARM_OP_##T##_ASR_REG = 0; \
   static const DynaCompiler ARM_OP_##T##_ROR_IMM = 0; \
   static const DynaCompiler ARM_OP_##T##_ROR_REG = 0; \
   static const DynaCompiler ARM_OP_##T##_IMM_VAL = 0

ARM_OP_IMM(AND, LSL_IMM)
ARM_OP_REG(AND, LSL_REG)
ARM_OP_IMM(AND, LSR_IMM)
ARM_OP_REG(AND, LSR_REG)
ARM_OP_IMM(AND, ASR_IMM)

static INSTR_R ARM_OP_AND_ASR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_AND_ROR_IMM (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_AND_ROR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_AND_IMM_VAL (uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_AND, pc, REG_POS(i,12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}


ARM_OP_IMM(EOR, LSL_IMM)
ARM_OP_REG(EOR, LSL_REG)
ARM_OP_IMM(EOR, LSR_IMM)
ARM_OP_REG(EOR, LSR_REG)
ARM_OP_IMM(EOR, ASR_IMM)
static INSTR_R ARM_OP_EOR_ASR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_EOR_ROR_IMM (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_EOR_ROR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_EOR_IMM_VAL (uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_EOR, pc, REG_POS(i,12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}


ARM_OP_IMM(ORR, LSL_IMM)
ARM_OP_REG(ORR, LSL_REG)
ARM_OP_IMM(ORR, LSR_IMM)
ARM_OP_REG(ORR, LSR_REG)
ARM_OP_IMM(ORR, ASR_IMM)
static INSTR_R ARM_OP_ORR_ASR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ORR_ROR_IMM (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ORR_ROR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ORR_IMM_VAL (uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_ORR, pc, REG_POS(i,12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}
 
ARM_OP_IMM(ADD, LSL_IMM)
ARM_OP_REG(ADD, LSL_REG)
ARM_OP_IMM(ADD, LSR_IMM)
ARM_OP_REG(ADD, LSR_REG)
ARM_OP_IMM(ADD, ASR_IMM)
static INSTR_R ARM_OP_ADD_ASR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADD_ROR_IMM (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADD_ROR_REG (uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADD_IMM_VAL (uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_ADD, pc, REG_POS(i,12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

ARM_OP_IMM(SUB, LSL_IMM)
ARM_OP_REG(SUB, LSL_REG)
ARM_OP_IMM(SUB, LSR_IMM) 
ARM_OP_REG(SUB, LSR_REG)
ARM_OP_IMM(SUB, ASR_IMM)
static INSTR_R ARM_OP_SUB_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SUB_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SUB_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SUB_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_SUB, pc, REG_POS(i,12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

ARM_OP_IMM(BIC, LSL_IMM)
ARM_OP_REG(BIC, LSL_REG)
ARM_OP_IMM(BIC, LSR_IMM) 
ARM_OP_REG(BIC, LSR_REG)
ARM_OP_IMM(BIC, ASR_IMM)
static INSTR_R ARM_OP_BIC_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_BIC_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_BIC_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_BIC_IMM_VAL(uint32_t pc, const u32 i)
{ 
   currentBlock.addOP(OP_AND, pc, REG_POS(i,12), REG_POS(i,16), -1, ~ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}



static INSTR_R ARM_OP_SBC_LSL_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_LSL_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_LSR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_LSR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_ASR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_IMM_VAL(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_SBC_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }



ARM_OP_IMM(RSB, LSL_IMM)
ARM_OP_REG(RSB, LSL_REG)
ARM_OP_IMM(RSB, LSR_IMM)
ARM_OP_REG(RSB, LSR_REG)
ARM_OP_IMM(RSB, ASR_IMM)
static INSTR_R ARM_OP_RSB_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSB_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSB_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSB_IMM_VAL(uint32_t pc, const u32 i) 
{
   currentBlock.addOP(OP_RSB, pc, REG_POS(i,12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}



static INSTR_R ARM_OP_RSC_LSL_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_LSL_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_LSR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_LSR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_ASR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_RSC_IMM_VAL(uint32_t pc, const u32 i) { return INTERPRET; }

static INSTR_R ARM_OP_ADC_LSL_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_LSL_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_LSR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_LSR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_ASR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ADC_IMM_VAL(uint32_t pc, const u32 i) { return INTERPRET; }
 
//-----------------------------------------------------------------------------
//   MOV
//-----------------------------------------------------------------------------
 
static INSTR_R ARM_OP_MOV_LSL_IMM (uint32_t pc, const u32 i)
{
   if (i == 0xE1A00000)
      currentBlock.addOP(OP_NOP, pc, 0, 0, 0, 0, PRE_OP_NONE, -1);
   else
      currentBlock.addOP(OP_MOV, pc, REG_POS(i,12), REG_POS(i,16), REG_POS(i, 0), ((i>>7)&0x1F), PRE_OP_LSL_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   
   return DYNAREC; 
}


ARM_OP_REG(MOV, LSL_REG)
ARM_OP_IMM(MOV, LSR_IMM)
ARM_OP_REG(MOV, LSR_REG)
ARM_OP_IMM(MOV, ASR_IMM)
static INSTR_R ARM_OP_MOV_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MOV_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MOV_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MOV_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_MOV, pc, REG_POS(i,12), -1, -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

//-----------------------------------------------------------------------------
//   MVN
//-----------------------------------------------------------------------------

ARM_OP_IMM(MVN, LSL_IMM)
ARM_OP_REG(MVN, LSL_REG)
ARM_OP_IMM(MVN, LSR_IMM)
ARM_OP_REG(MVN, LSR_REG)
ARM_OP_IMM(MVN, ASR_IMM)
static INSTR_R ARM_OP_MVN_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MVN_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MVN_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MVN_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_MOV, pc, REG_POS(i,12), -1, -1, ~ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}


//-----------------------------------------------------------------------------
//   CMP
//-----------------------------------------------------------------------------


#define use_flagOPS
#ifdef use_flagOPS
/*
ARM_OP_IMM_(CMP, LSL_IMM, -1)
ARM_OP_REG_(CMP, LSL_REG, -1)
ARM_OP_IMM_(CMP, LSR_IMM, -1)
ARM_OP_REG_(CMP, LSR_REG, -1)
ARM_OP_IMM_(CMP, ASR_IMM, -1)
static INSTR_R ARM_OP_CMP_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_CMP_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; } 
static INSTR_R ARM_OP_CMP_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_CMP_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_CMP, pc, -1, REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}*/

ARM_OP_UNDEF(CMP );
ARM_OP_UNDEF(TST );
/*/
ARM_OP_IMM_(TST, LSL_IMM, -1)
ARM_OP_REG_(TST, LSL_REG, -1)
ARM_OP_IMM_(TST, LSR_IMM, -1)
ARM_OP_REG_(TST, LSR_REG, -1)
ARM_OP_IMM_(TST, ASR_IMM, -1)
static INSTR_R ARM_OP_TST_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_TST_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_TST_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_TST_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_TST, pc, -1, REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}*/

disable_op(AND_S, LSL_IMM)
ARM_OP_REG(AND_S, LSL_REG)
ARM_OP_IMM(AND_S, LSR_IMM)
ARM_OP_REG(AND_S, LSR_REG)
ARM_OP_IMM(AND_S, ASR_IMM)
static INSTR_R ARM_OP_AND_S_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_AND_S_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_AND_S_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_AND_S_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_AND_S, pc, REG_POS(i, 12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

disable_op(ORR_S, LSL_IMM)
ARM_OP_REG(ORR_S, LSL_REG)
ARM_OP_IMM(ORR_S, LSR_IMM)
ARM_OP_REG(ORR_S, LSR_REG)
ARM_OP_IMM(ORR_S, ASR_IMM)
static INSTR_R ARM_OP_ORR_S_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ORR_S_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ORR_S_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_ORR_S_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_ORR_S, pc, REG_POS(i, 12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}


disable_op(EOR_S, LSL_IMM)
ARM_OP_REG(EOR_S, LSL_REG)
ARM_OP_IMM(EOR_S, LSR_IMM)
ARM_OP_REG(EOR_S, LSR_REG)
ARM_OP_IMM(EOR_S, ASR_IMM)
static INSTR_R ARM_OP_EOR_S_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_EOR_S_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_EOR_S_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_EOR_S_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_EOR_S, pc, REG_POS(i, 12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

disable_op(MOV_S, LSL_IMM)
ARM_OP_REG(MOV_S, LSL_REG)
ARM_OP_IMM(MOV_S, LSR_IMM)
ARM_OP_REG(MOV_S, LSR_REG)
ARM_OP_IMM(MOV_S, ASR_IMM)
static INSTR_R ARM_OP_MOV_S_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MOV_S_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MOV_S_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MOV_S_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_MOV_S, pc, REG_POS(i, 12), -1, -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

ARM_OP_IMM(MVN_S, LSL_IMM)
ARM_OP_REG(MVN_S, LSL_REG)
ARM_OP_IMM(MVN_S, LSR_IMM)
ARM_OP_REG(MVN_S, LSR_REG)
ARM_OP_IMM(MVN_S, ASR_IMM)
static INSTR_R ARM_OP_MVN_S_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MVN_S_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MVN_S_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static INSTR_R ARM_OP_MVN_S_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_MOV_S, pc, REG_POS(i, 12), -1, -1, ~ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

#else
   
   ARM_OP_UNDEF(AND_S);
   ARM_OP_UNDEF(EOR_S);
   ARM_OP_UNDEF(ORR_S);
   ARM_OP_UNDEF(MOV_S);
   ARM_OP_UNDEF(MVN_S);
   ARM_OP_UNDEF(CMP );
   ARM_OP_UNDEF(TST );
#endif

/*
ARM_OP_IMM(SUB_S, LSL_IMM)
ARM_OP_REG(SUB_S, LSL_REG)
ARM_OP_IMM(SUB_S, LSR_IMM)
ARM_OP_REG(SUB_S, LSR_REG)
ARM_OP_IMM(SUB_S, ASR_IMM)
static OP_RESULT ARM_OP_SUB_S_ASR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static OP_RESULT ARM_OP_SUB_S_ROR_IMM(uint32_t pc, const u32 i) { return INTERPRET; }
static OP_RESULT ARM_OP_SUB_S_ROR_REG(uint32_t pc, const u32 i) { return INTERPRET; }
static OP_RESULT ARM_OP_SUB_S_IMM_VAL(uint32_t pc, const u32 i) 
{ 
   currentBlock.addOP(OP_SUB_S, pc, REG_POS(i, 12), REG_POS(i,16), -1, ROR((i&0xFF), (i>>7)&0x1E), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}*/
 
//ARM_OP_UNDEF(AND_S);
//ARM_OP_UNDEF(EOR_S);
ARM_OP_UNDEF(SUB_S);
ARM_OP_UNDEF(RSB_S);
ARM_OP_UNDEF(ADD_S);
ARM_OP_UNDEF(ADC_S);
ARM_OP_UNDEF(SBC_S);
ARM_OP_UNDEF(RSC_S);
ARM_OP_UNDEF(TEQ  );
ARM_OP_UNDEF(CMN  );
//ARM_OP_UNDEF(ORR_S);
//ARM_OP_UNDEF(MOV_S);
ARM_OP_UNDEF(BIC_S);
//ARM_OP_UNDEF(MVN_S);

static INSTR_R ARM_OP_MUL(uint32_t pc, const u32 i) 
{
   currentBlock.addOP(OP_MUL, pc, REG_POS(i,16), REG_POS(i,0), REG_POS(i,8), -1, PRE_OP_NONE, instr_is_conditional(i) ? CONDITION(i) : -1);
   return DYNAREC; 
} 

static INSTR_R ARM_OP_MLA(uint32_t pc, const u32 i) {
   return INTERPRET;
}


static INSTR_R ARM_OP_UMULL(uint32_t pc, const u32 i) {
   return INTERPRET;
}

static INSTR_R ARM_OP_SMULL(uint32_t pc, const u32 i) {
   return INTERPRET;
}

static INSTR_R ARM_OP_UMLAL(uint32_t pc, const u32 i) {
   return INTERPRET;
}

#define ARM_OP_MUL_S 0
#define ARM_OP_MLA_S 0
#define ARM_OP_MLA_S 0
#define ARM_OP_MUL_S 0

#define ARM_OP_UMULL_S     0
#define ARM_OP_UMLAL_S     0
#define ARM_OP_SMULL_S     0
#define ARM_OP_SMLAL       0
#define ARM_OP_SMLAL_S     0

#define ARM_OP_SMUL_B_B    0
#define ARM_OP_SMUL_T_B    0
#define ARM_OP_SMUL_B_T    0
#define ARM_OP_SMUL_T_T    0

#define ARM_OP_SMLA_B_B    0
#define ARM_OP_SMLA_T_B    0
#define ARM_OP_SMLA_B_T    0
#define ARM_OP_SMLA_T_T    0

#define ARM_OP_SMULW_B     0
#define ARM_OP_SMULW_T     0
#define ARM_OP_SMLAW_B     0
#define ARM_OP_SMLAW_T     0

#define ARM_OP_SMLAL_B_B   0
#define ARM_OP_SMLAL_T_B   0
#define ARM_OP_SMLAL_B_T   0
#define ARM_OP_SMLAL_T_T   0

#define ARM_OP_QADD        0
#define ARM_OP_QSUB        0
#define ARM_OP_QDADD       0
#define ARM_OP_QDSUB       0

//#define ARM_OP_CLZ         0
static INSTR_R ARM_OP_CLZ(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_CLZ, pc, REG_POS(i,12), REG_POS(i,0), -1, -1, PRE_OP_NONE, instr_is_conditional(i) ? CONDITION(i) : -1);
   return DYNAREC; 
}

#define ARM_MEM_OP_DEF2(T, Q) \
   static const DynaCompiler ARM_OP_##T##_M_LSL_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_P_LSL_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_M_LSR_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_P_LSR_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_M_ASR_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_P_ASR_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_M_ROR_##Q = 0; \
   static const DynaCompiler ARM_OP_##T##_P_ROR_##Q = 0; \
   

#define ARM_MEM_OP_DEF(T) \
   ARM_MEM_OP_DEF2(T, IMM_OFF_PREIND); \
   ARM_MEM_OP_DEF2(T, IMM_OFF); \
   ARM_MEM_OP_DEF2(T, IMM_OFF_POSTIND)

ARM_MEM_OP_DEF(STR);
ARM_MEM_OP_DEF(LDR);
ARM_MEM_OP_DEF(STRB);
ARM_MEM_OP_DEF(LDRB); 


inline bool isMainMemory(u32 addr)
{
   return ((addr & 0x0F000000) == 0x02000000) && ((addr&(~0x3FFF)) != MMU.DTCMRegion);
}

inline bool isDTCM(u32 addr)
{
   return (addr<0x02000000) && block_procnum == ARM9;
}

static INSTR_R ARM_OP_STR_P_IMM_OFF_PREIND(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_STR, pc, REG_POS(i,16), REG_POS(i,12), -1,  ((i)&0xFFF), PRE_OP_PRE_P, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STR_M_IMM_OFF_PREIND(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_STR, pc, REG_POS(i,16), REG_POS(i,12), -1,  ((i)&0xFFF), PRE_OP_PRE_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STR_M_IMM_OFF(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_STR, pc, REG_POS(i,16), REG_POS(i,12), -1, -((i)&0xFFF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STR_P_IMM_OFF(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_STR, pc, REG_POS(i,16), REG_POS(i,12), -1, ((i)&0xFFF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STR_M_IMM_OFF_POSTIND(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_STR, pc, REG_POS(i,16), REG_POS(i,12), -1,  ((i)&0xFFF), PRE_OP_POST_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STR_P_IMM_OFF_POSTIND(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_STR, pc, REG_POS(i,16), REG_POS(i,12), -1,  ((i)&0xFFF), PRE_OP_POST_P, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDR_M_IMM_OFF_PREIND(uint32_t pc, const u32 i){

   if (REG_POS(i,12) == 15)
      return INTERPRET;

   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, ((i)&0xFFF), PRE_OP_PRE_M, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDR_P_IMM_OFF_PREIND(uint32_t pc, const u32 i){

   if (REG_POS(i,12) == 15)
      return INTERPRET;

   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, ((i)&0xFFF), PRE_OP_PRE_P, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDR_M_IMM_OFF(uint32_t pc, const u32 i){

   if (REG_POS(i,12) == 15) 
      return INTERPRET;

   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, -((i)&0xFFF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDR_P_IMM_OFF(uint32_t pc, const u32 i){

   if (REG_POS(i,12) == 15)
      return INTERPRET;

   u32 addr = _ARMPROC.R[REG_POS(i,16)] + ((i)&0xFFF);

   /*if (isDTCM(addr))
      currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, ((i)&0xFFF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1, EXTFL_DIRECTDTCM);
   else*/
      currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, ((i)&0xFFF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDR_M_IMM_OFF_POSTIND(uint32_t pc, const u32 i){

   if (REG_POS(i,12) == 15)
      return INTERPRET;

   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, ((i)&0xFFF), PRE_OP_POST_M, instr_is_conditional(i) ? CONDITION(i) : -1,isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDR_P_IMM_OFF_POSTIND(uint32_t pc, const u32 i){

   if (REG_POS(i,12) == 15)
      return INTERPRET;

   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_LDR, pc, REG_POS(i,12), REG_POS(i,16), -1, ((i)&0xFFF), PRE_OP_POST_P, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static const DynaCompiler ARM_OP_STRB_M_IMM_OFF_PREIND = 0;
static const DynaCompiler ARM_OP_STRB_P_IMM_OFF_PREIND = 0;
static const DynaCompiler ARM_OP_STRB_M_IMM_OFF = 0;
static const DynaCompiler ARM_OP_STRB_P_IMM_OFF = 0;
static const DynaCompiler ARM_OP_STRB_M_IMM_OFF_POSTIND = 0;
static const DynaCompiler ARM_OP_STRB_P_IMM_OFF_POSTIND = 0;

static const DynaCompiler ARM_OP_LDRB_M_IMM_OFF_PREIND = 0;
static const DynaCompiler ARM_OP_LDRB_P_IMM_OFF_PREIND = 0;
static const DynaCompiler ARM_OP_LDRB_M_IMM_OFF = 0;
static const DynaCompiler ARM_OP_LDRB_P_IMM_OFF = 0;
static const DynaCompiler ARM_OP_LDRB_M_IMM_OFF_POSTIND = 0;
static const DynaCompiler ARM_OP_LDRB_P_IMM_OFF_POSTIND = 0;


#define ARM_MEM_HALF_OP_DEF2(T, P) \
   static const DynaCompiler ARM_OP_##T##_##P##M_REG_OFF = 0; \
   static const DynaCompiler ARM_OP_##T##_##P##P_REG_OFF = 0; \
   static const DynaCompiler ARM_OP_##T##_##P##M_IMM_OFF = 0; \
   static const DynaCompiler ARM_OP_##T##_##P##P_IMM_OFF = 0

#define ARM_MEM_HALF_OP_DEF(T) \
   ARM_MEM_HALF_OP_DEF2(T, POS_INDE_); \
   ARM_MEM_HALF_OP_DEF2(T, ); \
   ARM_MEM_HALF_OP_DEF2(T, PRE_INDE_)


//ARM_MEM_HALF_OP_DEF(STRH);
//ARM_MEM_HALF_OP_DEF(LDRH);
ARM_MEM_HALF_OP_DEF(STRSB); 
ARM_MEM_HALF_OP_DEF(LDRSB);
ARM_MEM_HALF_OP_DEF(STRSH);
ARM_MEM_HALF_OP_DEF(LDRSH);

static INSTR_R ARM_OP_STRH_P_IMM_OFF(uint32_t pc, const u32 i)
{
   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i,12), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,  isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}
static INSTR_R ARM_OP_STRH_M_IMM_OFF(uint32_t pc, const u32 i)
{
   u32 addr = _ARMPROC.R[REG_POS(i,16)];

   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i,12), -1, -(((i>>4)&0xF0)+(i&0xF)), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1,  isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STRH_P_REG_OFF(uint32_t pc, const u32 i)
{   
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i, 12), REG_POS(i,0), -1, PRE_OP_REG_OFF, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_STRH_M_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 0), REG_POS(i, 16), REG_POS(i,12), -1, PRE_OP_REG_PRE_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_STRH_PRE_INDE_P_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i,12), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_PRE_P, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_STRH_PRE_INDE_M_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i,12), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_PRE_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_STRH_PRE_INDE_P_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_STRH_PRE_INDE_M_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_STRH_POS_INDE_P_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i,12), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_POST_P, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_STRH_POS_INDE_M_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, REG_POS(i, 16), REG_POS(i,12), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_POST_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_STRH_POS_INDE_P_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_STRH_POS_INDE_M_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


//LDRH
static INSTR_R ARM_OP_LDRH_P_IMM_OFF(uint32_t pc, const u32 i)
{
   u32 addr = _ARMPROC.R[REG_POS(i, 16)] + (((i>>4)&0xF0)+(i&0xF));
   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}
static INSTR_R ARM_OP_LDRH_M_IMM_OFF(uint32_t pc, const u32 i)
{
   u32 addr = _ARMPROC.R[REG_POS(i, 16)] - (((i>>4)&0xF0)+(i&0xF));

   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), -1, -(((i>>4)&0xF0)+(i&0xF)), PRE_OP_IMM, instr_is_conditional(i) ? CONDITION(i) : -1, isMainMemory(addr) ? EXTFL_DIRECTMEMACCESS : EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDRH_P_REG_OFF(uint32_t pc, const u32 i)
{   
   return INTERPRET;
   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), REG_POS(i, 0), 0, PRE_OP_REG_OFF, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC; 
}

static INSTR_R ARM_OP_LDRH_M_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_LDRH_PRE_INDE_P_IMM_OFF(uint32_t pc, const u32 i)
{
    return INTERPRET;
   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_PRE_P, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_LDRH_PRE_INDE_M_IMM_OFF(uint32_t pc, const u32 i)
{
    return INTERPRET;
   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_PRE_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_LDRH_PRE_INDE_P_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_LDRH_PRE_INDE_M_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_LDRH_POS_INDE_P_IMM_OFF(uint32_t pc, const u32 i)
{
    return INTERPRET;
   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_POST_P, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_LDRH_POS_INDE_M_IMM_OFF(uint32_t pc, const u32 i)
{
    return INTERPRET;
   currentBlock.addOP(OP_LDRH, pc, REG_POS(i,12), REG_POS(i, 16), -1, ((i>>4)&0xF0)+(i&0xF), PRE_OP_POST_M, instr_is_conditional(i) ? CONDITION(i) : -1,EXTFL_NONE);
   return DYNAREC;
}

static INSTR_R ARM_OP_LDRH_POS_INDE_P_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R ARM_OP_LDRH_POS_INDE_M_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

#define SIGNEXTEND_24(i) (((s32)i<<8)>>8)

static INSTR_R ARM_OP_B(uint32_t pc, const u32 i)
{
   u32 tmp = _ARMPROC.R[15];
   u32 off = SIGNEXTEND_24(i);
   bool _thumb = (CONDITION(i)==0xF);

	tmp += (off<<2);

	currentBlock.branch_addr = tmp & (0xFFFFFFFC|(BIT0(tmp)<<1));

   //if (currentBlock.branch_addr == currentBlock.start_addr) printf("WARNING: B to self\n");
   return INTERPRET;
}

#define ARM_OP_BL 0

//-----------------------------------------------------------------------------
//   MRS / MSR
//-----------------------------------------------------------------------------
static INSTR_R ARM_OP_MRS_CPSR(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static INSTR_R ARM_OP_MRS_SPSR(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


//static OP_RESULT ARM_OP_BKPT(uint32_t pc, const u32 i) { emit_prefetch(); return OPR_RESULT(OPR_CONTINUE, 1); }

//-----------------------------------------------------------------------------
//   SWP/SWPB
//-----------------------------------------------------------------------------

#define _ARMPROC (block_procnum ? NDS_ARM7:NDS_ARM9)

static INSTR_R ARM_OP_SWP (uint32_t pc, const u32 i) { return INTERPRET;  };
static INSTR_R ARM_OP_SWPB(uint32_t pc, const u32 i) { return INTERPRET;  };

static INSTR_R ARM_OP_MCR(uint32_t pc, const u32 i){ 
   return INTERPRET;
}



static INSTR_R ARM_OP_SWI(uint32_t pc, const u32 i){
   currentBlock.addOP(OP_SWI, pc, -1, ((i>>16)&0x1F), -1, -1, PRE_OP_NONE, instr_is_conditional(i) ? CONDITION(i) : -1);
   return DYNAREC;
}

static INSTR_R ARM_OP_BX(uint32_t pc, const u32 i){
   u32 tmp = _ARMPROC.R[REG_POS(i, 0)];
	currentBlock.branch_addr = tmp & (0xFFFFFFFC|(BIT0(tmp)<<1));

   if (currentBlock.branch_addr == currentBlock.start_addr) printf("WARNING: BX to self\n");
   return INTERPRET;
}

//-----------------------------------------------------------------------------
//   LDRD / STRD
//-----------------------------------------------------------------------------

static INSTR_R ARM_OP_LDRD_STRD_POST_INDEX(uint32_t pc, const u32 i){
   return INTERPRET;
}

static INSTR_R ARM_OP_STMIA(uint32_t pc, const u32 i){
   return INTERPRET;
   currentBlock.addOP(OP_STMIA, pc, -1, i, REG_POS(i,16), -1, PRE_OP_NONE, instr_is_conditional(i) ? CONDITION(i) : -1);
   return DYNAREC;
}


#define ARM_OP_LDRD_STRD_OFFSET_PRE_INDEX 0
#define ARM_OP_MSR_CPSR 0
#define ARM_OP_BX 0
#define ARM_OP_BLX_REG 0
#define ARM_OP_BKPT 0
#define ARM_OP_MSR_SPSR 0
#define ARM_OP_STREX 0
#define ARM_OP_LDREX 0
#define ARM_OP_MSR_CPSR_IMM_VAL 0
#define ARM_OP_MSR_SPSR_IMM_VAL 0
#define ARM_OP_STMDA 0
#define ARM_OP_LDMDA 0
#define ARM_OP_STMDA_W 0
#define ARM_OP_LDMDA_W 0
#define ARM_OP_STMDA2 0
#define ARM_OP_LDMDA2 0
#define ARM_OP_STMDA2_W 0
#define ARM_OP_LDMDA2_W 0
//#define ARM_OP_STMIA 0
#define ARM_OP_LDMIA 0
#define ARM_OP_STMIA_W 0
#define ARM_OP_LDMIA_W 0
#define ARM_OP_STMIA2 0
#define ARM_OP_LDMIA2 0
#define ARM_OP_STMIA2_W 0
#define ARM_OP_LDMIA2_W 0
#define ARM_OP_STMDB 0
#define ARM_OP_LDMDB 0
#define ARM_OP_STMDB_W 0
#define ARM_OP_LDMDB_W 0
#define ARM_OP_STMDB2 0
#define ARM_OP_LDMDB2 0
#define ARM_OP_STMDB2_W 0
#define ARM_OP_LDMDB2_W 0
#define ARM_OP_STMIB 0
#define ARM_OP_LDMIB 0
#define ARM_OP_STMIB_W 0
#define ARM_OP_LDMIB_W 0
#define ARM_OP_STMIB2 0
#define ARM_OP_LDMIB2 0
#define ARM_OP_STMIB2_W 0
#define ARM_OP_LDMIB2_W 0
#define ARM_OP_STC_OPTION 0
#define ARM_OP_LDC_OPTION 0
#define ARM_OP_STC_M_POSTIND 0
#define ARM_OP_LDC_M_POSTIND 0
#define ARM_OP_STC_P_POSTIND 0
#define ARM_OP_LDC_P_POSTIND 0
#define ARM_OP_STC_M_IMM_OFF 0
#define ARM_OP_LDC_M_IMM_OFF 0
#define ARM_OP_STC_M_PREIND 0
#define ARM_OP_LDC_M_PREIND 0
#define ARM_OP_STC_P_IMM_OFF 0
#define ARM_OP_LDC_P_IMM_OFF 0
#define ARM_OP_STC_P_PREIND 0
#define ARM_OP_LDC_P_PREIND 0
#define ARM_OP_CDP 0
#define ARM_OP_MRC 0
#define ARM_OP_UND 0
static const DynaCompiler arm_instruction_compilers[4096] = {
#define TABDECL(x) ARM_##x
#include "instruction_tabdef.inc"
#undef TABDECL
};

////////
// THUMB
////////

static INSTR_R THUMB_OP_ASR(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_LSL_0(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_MOV, pc, _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_LSL(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static INSTR_R THUMB_OP_LSR_0(uint32_t pc, const u32 i)
{
   printf("OP_LSR_0 not tested\n");
   currentBlock.addOP(OP_LSR_0, pc, _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_LSR(uint32_t pc, const u32 i)
{
   return INTERPRET; 
}


static INSTR_R THUMB_OP_MOV_IMM8(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_MOV, pc, _REG_NUM(i, 8), -1, -1, i&0xff, PRE_OP_IMM);
   return DYNAREC;
}


static INSTR_R THUMB_OP_ADD_IMM8(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_ADD, pc, _REG_NUM(i, 8), _REG_NUM(i, 8), -1, i&0xff, PRE_OP_IMM);
   return DYNAREC;
}

static INSTR_R THUMB_OP_ADD_IMM3(uint32_t pc, const u32 i)
{
   u32 imm = (i>>6)&0x07;
   if (imm == 0)
      currentBlock.addOP(OP_MOV, pc, _REG_NUM(i, 0), _REG_NUM(i, 3));
   else
      currentBlock.addOP(OP_ADD, pc, _REG_NUM(i, 0), _REG_NUM(i, 3), -1, imm, PRE_OP_IMM);
   return DYNAREC;
}

static INSTR_R THUMB_OP_ADD_REG(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_ADD, pc, _REG_NUM(i, 0), _REG_NUM(i, 3), _REG_NUM(i, 6));
   return DYNAREC;
}

static INSTR_R THUMB_OP_SUB_REG(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_SUB, pc, _REG_NUM(i, 0), _REG_NUM(i, 3), _REG_NUM(i, 6));
   return DYNAREC;
}

static INSTR_R THUMB_OP_CMP_IMM8(uint32_t pc, const u32 i)
{
   return INTERPRET; 
   currentBlock.addOP(OP_CMP, pc, -1, _REG_NUM(i, 8), -1, i&0xff, PRE_OP_IMM);
   return DYNAREC; 
}

static INSTR_R THUMB_OP_SUB_IMM8(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_SUB, pc, _REG_NUM(i, 8), _REG_NUM(i, 8), -1, i&0xff, PRE_OP_IMM);
   return DYNAREC;
}

static INSTR_R THUMB_OP_SUB_IMM3(uint32_t pc, const u32 i)
{
   u32 imm = (i>>6)&0x07;
   currentBlock.addOP(OP_SUB, pc, _REG_NUM(i, 0), _REG_NUM(i, 3), -1, imm, PRE_OP_IMM);
   return DYNAREC;
}

//-----------------------------------------------------------------------------
//   AND
//-----------------------------------------------------------------------------

static INSTR_R THUMB_OP_AND(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_AND, pc, _REG_NUM(i, 0), _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_BIC(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static INSTR_R THUMB_OP_CMN(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_NEG(uint32_t pc, const u32 i)
{
   return INTERPRET; 
   currentBlock.addOP(OP_NEG, pc, _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}



//-----------------------------------------------------------------------------
//   EOR
//-----------------------------------------------------------------------------

static INSTR_R THUMB_OP_EOR(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_EOR, pc, _REG_NUM(i, 0), _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

//-----------------------------------------------------------------------------
//   MVN
//-----------------------------------------------------------------------------

static INSTR_R THUMB_OP_MVN(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_MVN, pc, _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_ORR(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_ORR, pc, _REG_NUM(i, 0), _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

//-----------------------------------------------------------------------------
//   TST
//-----------------------------------------------------------------------------
static INSTR_R THUMB_OP_TST(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_TST, pc, -1, _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_CMP(uint32_t pc, const u32 i)
{
   return INTERPRET; 
   currentBlock.addOP(OP_CMP, pc, -1, _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_ROR_REG(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_MOV_SPE(uint32_t pc, const u32 i)
{
   u32 Rd = _REG_NUM(i, 0) | ((i>>4)&8);
   
   currentBlock.addOP(OP_MOV, pc, Rd, REG_POS(i, 3));
   return DYNAREC; 
}

static INSTR_R THUMB_OP_ADD_SPE(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_MUL_REG(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_MUL, pc, _REG_NUM(i, 0), _REG_NUM(i, 0), _REG_NUM(i, 3));
   return DYNAREC; 
}


static INSTR_R THUMB_OP_CMP_SPE(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_B_COND(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_B_UNCOND(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_ADJUST_P_SP(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_ADD, pc, 13, 13, -1, ((i&0x7F)<<2), PRE_OP_IMM, EXTFL_NOFLAGS);
   return DYNAREC;
}

static INSTR_R THUMB_OP_ADJUST_M_SP(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_ADD, pc, 13, 13, -1, -((i&0x7F)<<2), PRE_OP_IMM, EXTFL_NOFLAGS);
   return DYNAREC;
} 

static INSTR_R THUMB_OP_ADD_2PC(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_ADD_2SP(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_ADD, pc, _REG_NUM(i, 8), 13, -1, ((i&0xFF)<<2), PRE_OP_IMM, EXTFL_NOFLAGS);
   return DYNAREC;
}

static INSTR_R THUMB_OP_BL_10(uint32_t pc, const u32 i)
{
   return INTERPRET; 
}

static INSTR_R THUMB_OP_POP(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_PUSH(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_SWI_THUMB(uint32_t pc, const u32 i)
{
   currentBlock.addOP(OP_SWI, pc, -1, i&0x1F);
   return DYNAREC;
}

static INSTR_R THUMB_OP_STRB_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_LDRB_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static INSTR_R THUMB_OP_STRH_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STRH, pc, _REG_NUM(i, 3), _REG_NUM(i,0), _REG_NUM(i,6), 0, PRE_OP_REG_OFF, -2);
}

static INSTR_R THUMB_OP_LDRH_REG_OFF(uint32_t pc, const u32 i)
{
   u32 adr = _ARMPROC.R[_REG_NUM(i, 3)] + _ARMPROC.R[_REG_NUM(i, 6)];

   return INTERPRET;
   /*if (isMainMemory(adr))
      currentBlock.addOP(OP_LDRH, pc, _REG_NUM(i,0), _REG_NUM(i, 3), _REG_NUM(i, 6), -1, PRE_OP_REG_OFF, -1, EXTFL_DIRECTMEMACCESS);
   else*/
      currentBlock.addOP(OP_LDRH, pc, _REG_NUM(i,0), _REG_NUM(i, 3), _REG_NUM(i, 6), -1, PRE_OP_REG_OFF);
}

static INSTR_R THUMB_OP_STRB_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_LDRB_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_STRH_IMM_OFF(uint32_t pc, const u32 i)
{
   u32 adr = _ARMPROC.R[_REG_NUM(i, 3)] + ((i>>5)&0x3E);

   if (isMainMemory(adr))
      currentBlock.addOP(OP_STRH, pc, _REG_NUM(i, 3), _REG_NUM(i, 0), -1, ((i>>5)&0x3E), PRE_OP_IMM, -2, EXTFL_DIRECTMEMACCESS);
   else
      currentBlock.addOP(OP_STRH, pc, _REG_NUM(i, 3), _REG_NUM(i, 0), -1, ((i>>5)&0x3E), PRE_OP_IMM, -2);
   return DYNAREC;
}

static INSTR_R THUMB_OP_LDRH_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   u32 adr = _ARMPROC.R[_REG_NUM(i, 3)] + ((i>>5)&0x3E);

   currentBlock.addOP(OP_LDRH, pc, _REG_NUM(i,0), _REG_NUM(i, 3), -1, ((i>>5)&0x3E), PRE_OP_IMM);
   return DYNAREC;
}


static INSTR_R THUMB_OP_STR_IMM_OFF(uint32_t pc, const u32 i)
{
   u32 adr = _ARMPROC.R[_REG_NUM(i, 3)] + ((i>>4)&0x7C);

   return INTERPRET;

   /*if (isMainMemory(adr))
      currentBlock.addOP(OP_STR, pc, _REG_NUM(i, 3), _REG_NUM(i, 0), -1, ((i>>4)&0x7C), PRE_OP_IMM, -1, EXTFL_DIRECTMEMACCESS);
   else*/
      currentBlock.addOP(OP_STR, pc, _REG_NUM(i, 3), _REG_NUM(i, 0), -1, ((i>>4)&0x7C), PRE_OP_IMM);
   return DYNAREC;
}

static INSTR_R THUMB_OP_STR_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_STR, pc, _REG_NUM(i, 3), _REG_NUM(i, 0), _REG_NUM(i, 6), -1, PRE_OP_REG_OFF);
   return DYNAREC;
}

static INSTR_R THUMB_OP_LDR_REG_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_LDR, pc, _REG_NUM(i, 0), _REG_NUM(i, 3), _REG_NUM(i, 6), -1, PRE_OP_REG_OFF);
   return DYNAREC;
}

static INSTR_R THUMB_OP_LDR_IMM_OFF(uint32_t pc, const u32 i)
{
   return INTERPRET;
   currentBlock.addOP(OP_LDR, pc, _REG_NUM(i, 0), _REG_NUM(i, 3), -1, ((i>>4)&0x7C), PRE_OP_IMM);
   return DYNAREC;
}



static INSTR_R THUMB_OP_BL_11(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static INSTR_R THUMB_OP_BLX(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_BLX_THUMB(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static INSTR_R THUMB_OP_BX_THUMB(uint32_t pc, const u32 i)
{
   return INTERPRET;
}



#define THUMB_OP_INTERPRET       0

#define THUMB_OP_ASR             THUMB_OP_INTERPRET

#define THUMB_OP_UND_THUMB       THUMB_OP_INTERPRET

#define THUMB_OP_ASR_0           THUMB_OP_INTERPRET

#define THUMB_OP_LSL_REG         THUMB_OP_INTERPRET
#define THUMB_OP_LSR_REG         THUMB_OP_INTERPRET
#define THUMB_OP_ASR_REG         THUMB_OP_INTERPRET
#define THUMB_OP_ADC_REG         THUMB_OP_INTERPRET
#define THUMB_OP_SBC_REG         THUMB_OP_INTERPRET
#define THUMB_OP_ROR_REG         THUMB_OP_INTERPRET

#define THUMB_OP_LDR_SPREL       THUMB_OP_INTERPRET
#define THUMB_OP_STR_SPREL       THUMB_OP_INTERPRET
#define THUMB_OP_LDR_PCREL       THUMB_OP_INTERPRET


#define THUMB_OP_LDRSB_REG_OFF   THUMB_OP_INTERPRET
#define THUMB_OP_LDRSH_REG_OFF   THUMB_OP_INTERPRET


// UNDEFINED OPS
#define THUMB_OP_PUSH_LR         THUMB_OP_INTERPRET
#define THUMB_OP_BKPT_THUMB      THUMB_OP_INTERPRET

static INSTR_R THUMB_OP_POP_PC(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_STMIA_THUMB(uint32_t pc, const u32 i)
{
   return INTERPRET;
}

static INSTR_R THUMB_OP_LDMIA_THUMB(uint32_t pc, const u32 i)
{
   return INTERPRET;
}


static const DynaCompiler thumb_instruction_compilers[1024] = {
#define TABDECL(x) THUMB_##x
#include "thumb_tabdef.inc"
#undef TABDECL
};

// ============================================================================================= IMM

//-----------------------------------------------------------------------------
//   Generic instruction wrapper
//-----------------------------------------------------------------------------

template<int PROCNUM, int thumb>
static u32 FASTCALL OP_DECODE()
{
	u32 cycles;
	u32 adr = ARMPROC.instruct_adr;
	if(thumb)
	{
		ARMPROC.next_instruction = adr + 2;
		ARMPROC.R[15] = adr + 4;
		u32 opcode = _MMU_read16<PROCNUM, MMU_AT_CODE>(adr);
		cycles = thumb_instructions_set[PROCNUM][opcode>>6](opcode);
	}
	else
	{
		ARMPROC.next_instruction = adr + 4;
		ARMPROC.R[15] = adr + 8;
		u32 opcode = _MMU_read32<PROCNUM, MMU_AT_CODE>(adr);
		if(CONDITION(opcode) == 0xE || TEST_COND(CONDITION(opcode), CODE(opcode), ARMPROC.CPSR))
			cycles = arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(opcode)](opcode);
		else
			cycles = 1;
	}
	ARMPROC.instruct_adr = ARMPROC.next_instruction;
	return cycles;
}

static const ArmOpCompiled op_decode[2][2] = { OP_DECODE<0,0>, OP_DECODE<0,1>, OP_DECODE<1,0>, OP_DECODE<1,1> };


bool instr_does_prefetch(u32 opcode)
{
	u32 x = instr_attributes(opcode);
	if(thumb)
		return thumb_instruction_compilers[opcode>>6]
			   && (x & BRANCH_ALWAYS);
	else
		return instr_is_branch(opcode) && arm_instruction_compilers[INSTRUCTION_INDEX(opcode)]
			   && ((x & BRANCH_ALWAYS) || (x & BRANCH_LDM));
}


//-----------------------------------------------------------------------------
//   Compiler
//-----------------------------------------------------------------------------

void emit_checkblock_count(){
   printf("block here\n");
}

template<int PROCNUM>
static u32 compile_basicblock()
{
   uint32_t opcode = 0;
   void* code_ptr = emit_GetPtr();

   emit_mpush(4,
				reg_gpr+psp_gp,
				reg_gpr+psp_k0,
				reg_gpr+psp_fp,
				reg_gpr+psp_ra);
   
   if (thumb)
      emit_mpush(5,
               reg_gpr+psp_s0,
               reg_gpr+psp_s1,
               reg_gpr+psp_s2,
               reg_gpr+psp_s3,
               reg_gpr+psp_s4);

   StartCodeDump();

   emit_li(psp_k0,((u32)&ARMPROC), 2);

   if (thumb){
      if (currentBlock.emitThumbBlock<PROCNUM>()) //idle loop
         interpreted_cycles *= 10;
   }else{
      if (currentBlock.emitArmBlock<PROCNUM>()) //idle loop
         interpreted_cycles *= 100;
   }

   /*if (strcmp(">:1:03:228FFE20:8DA0D787:049532BF:1CCCFF37:46FE1542", currentBlock.block_hash) == 0) {
      //emit_jal(emit_checkblock_count);
      //emit_nop();
      CodeDump("1_04_dump.bin");
      die("Code dump\n");
   }*/ 

   if (currentBlock.manualPrefetch) 
   {
      emit_lw(psp_at, RCPU, _next_instr);
      emit_sw(psp_at, RCPU, _instr_adr);
   }

   if (thumb)
      emit_mpop(5,
               reg_gpr+psp_s0,
               reg_gpr+psp_s1,
               reg_gpr+psp_s2,
               reg_gpr+psp_s3,
               reg_gpr+psp_s4);

   emit_mpop(4,
				reg_gpr+psp_gp,
				reg_gpr+psp_k0,
				reg_gpr+psp_fp,
				reg_gpr+psp_ra);
   
   emit_jra();
   emit_movi(psp_v0, interpreted_cycles);
   
   make_address_range_executable((u32)code_ptr, (u32)emit_GetPtr());
   JIT_COMPILED_FUNC(base_adr, PROCNUM) = (uintptr_t)code_ptr;

   return interpreted_cycles;
}


#define _SHA1_DIGEST_LENGTH 5

template<int PROCNUM>
void build_ArmBasicblock(){

   uint32_t opnum = 0;
   const u32 isize = 4;
   const uint32_t imask = 0xFFFFFFFC;

   uint32_t digest[_SHA1_DIGEST_LENGTH];

	SceKernelUtilsSha1Context ctx;
	sceKernelUtilsSha1BlockInit(&ctx);

   bool contains_intr = false;

   for (uint32_t has_ended = 0; has_ended == 0; opnum ++, pc += isize){
      
      uint32_t op = _MMU_read32<PROCNUM, MMU_AT_CODE>(pc&imask);

      DynaCompiler fc = arm_instruction_compilers[INSTRUCTION_INDEX(op)];
      INSTR_R res = fc == 0 ? INTERPRET : fc(pc,op);;

      has_ended = instr_is_branch(op) || (opnum >= (my_config.DynarecBlockSize - 1));

      if (res == INTERPRET) {
         currentBlock.addOP(OP_ITP, pc, -1, op, -1, -1, PRE_OP_NONE, instr_is_conditional(op) ? CONDITION(op) : -1);
         if (!has_ended) contains_intr = true;
      }
      

      sceKernelUtilsSha1BlockUpdate(&ctx, (u8*)&op, 4);
      
      if (has_ended && (!instr_does_prefetch(op) || res == INTERPRET)) //prefetch next instruction
         currentBlock.manualPrefetch = true;

      interpreted_cycles += op_decode[PROCNUM][false]();
   }

   sceKernelUtilsSha1BlockResult(&ctx, (u8*)digest);

	sprintf(currentBlock.block_hash,">:1:%02X:%08X:%08X:%08X:%08X:%08X",opnum, digest[0]
																			, digest[1]
																			, digest[2]
																			, digest[3]
																			, digest[4]);
   
   //#define save_hash
   #ifdef save_hash
      extern void WriteHash(char* msg);
      if (!contains_intr) WriteHash(currentBlock.block_hash);
   #endif
   //printf("Block hash: %s\n", currentBlock.block_hash);
}

template<int PROCNUM>
void build_ThumbBasicblock(){

   const u32 isize = 2;
   const uint32_t imask = 0xFFFFFFFE;

   for (uint32_t i = 0, has_ended = 0; has_ended == 0; i ++, pc += isize){

      uint32_t op = _MMU_read16<PROCNUM, MMU_AT_CODE>(pc&imask);
   
      DynaCompiler fc = thumb_instruction_compilers[op>>6];
      INSTR_R res = fc == 0 ? INTERPRET : fc(pc,op);

      if (res == INTERPRET)
         currentBlock.addOP(OP_ITP, pc, -1, op, -1, -1, PRE_OP_NONE, false);

      has_ended = instr_is_branch(op) || (i >= (my_config.DynarecBlockSize - 1));
      
      if (has_ended && (!instr_does_prefetch(op) || res == INTERPRET)) //prefetch next instruction
         currentBlock.manualPrefetch = true;

      interpreted_cycles += op_decode[PROCNUM][true]();
   }
}

template<int PROCNUM> u32 arm_jit_compile()
{

   // prevent endless recompilation of self-modifying code, which would be a memleak since we only free code all at once.
	// also allows us to clear compiled_funcs[] while leaving it sparsely allocated, if the OS does memory overcommit.
	block_procnum = PROCNUM;
   interpreted_cycles = 0;

   thumb = ARMPROC.CPSR.bits.T == 1;
   base_adr = ARMPROC.instruct_adr;
   pc = base_adr; 

   currentBlock.start_addr = base_adr;

	/*u32 mask_adr = (base_adr & 0x07FFFFFE) >> 4;
	if(((recompile_counts[mask_adr >> 1] >> 4*(mask_adr & 1)) & 0xF) > 8)
	{
		ArmOpCompiled f = op_decode[PROCNUM][thumb];
		JIT_COMPILED_FUNC(base_adr, PROCNUM) = (uintptr_t)f;
		return f();
	}
	recompile_counts[mask_adr >> 1] += 1 << 4*(mask_adr & 1);*/

   if (GetFreeSpace() < 16 * 1024){
      //printf("Dynarec code reset\n");
      arm_jit_reset(true,true);
   }

   currentBlock.clearBlock(); 

   if (!thumb) {
      build_ArmBasicblock<PROCNUM>(); 
      currentBlock.optimize_basicblock();
      /*ArmOpCompiled f = op_decode[PROCNUM][thumb];
		JIT_COMPILED_FUNC(base_adr, PROCNUM) = (uintptr_t)f;
		return f();*/
   }else{
      build_ThumbBasicblock<PROCNUM>();
      currentBlock.optimize_basicblockThumb();
   }

   return compile_basicblock<PROCNUM>();
}

template u32 arm_jit_compile<0>();
template u32 arm_jit_compile<1>();

void arm_jit_reset(bool enable, bool suppress_msg)
{
   if (!suppress_msg)
	   printf("CPU mode: %s\n", enable?"JIT":"Interpreter");

   saveBlockSizeJIT = my_config.DynarecBlockSize; //CommonSettings.jit_max_block_size;

   if (enable)
   {
      //printf("JIT: max block size %d instruction(s)\n", CommonSettings.jit_max_block_size);

      #define JITFREE(x) memset(x,0,sizeof(x));
         JITFREE(JIT.MAIN_MEM);
         JITFREE(JIT.SWIRAM);
         JITFREE(JIT.ARM9_ITCM);
         JITFREE(JIT.ARM9_LCDC);
         JITFREE(JIT.ARM9_BIOS);
         JITFREE(JIT.ARM7_BIOS);
         JITFREE(JIT.ARM7_ERAM);
         JITFREE(JIT.ARM7_WIRAM);
         JITFREE(JIT.ARM7_WRAM);
      #undef JITFREE

      //memset(recompile_counts, 0, sizeof(recompile_counts));
      init_jit_mem();

     resetCodeCache();
   }
}

void arm_jit_close()
{
   resetCodeCache();
}