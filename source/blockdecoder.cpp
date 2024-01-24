#include "blockdecoder.h"

#include "mips_code_emiter.h"
#include "PSP/emit/psp_emit.h"
#include "armcpu.h"

#include "Disassembler.h"

#include <functional>

bool instr_does_prefetch(u32 opcode);

inline void loadReg(psp_gpr_t psp_reg, s32 nds_reg) { if (nds_reg != -1) emit_lw(psp_reg, RCPU, _reg(nds_reg)); }
inline void storeReg(psp_gpr_t psp_reg, s32 nds_reg) { if (nds_reg != -1) emit_sw(psp_reg, RCPU, _reg(nds_reg)); }

#define offsetBetween(x, x2) (((u32)&x) - ((u32)&x2[0]))
#define offsetBetween2(x, x2) (((u32)&x2[0]) - ((u32)&x))

u32 mem_off = 0;
u32 dtcm_addr_arr = 0;
int intr_instr = 0;

u32 start_block = 0;

#include "psp_sra.h"

block currentBlock;  
reg_allocation reg_alloc;

const char * compiled_functions_hash = 
    ">:1:05:8FDA19B2:C2AF4F28:998B99C6:F0596DB3:2FD2AA9B" //used by yoshi island ingame
    ">:1:07:88DD1AED:096B3BDA:7857790C:6D443C78:A4ADDB58" //a lot used in pokemon diamond 
    ;

//make an array of pointers to functions of type void 
//and pass the opcode to the function
std::function<void(psp_gpr_t psp_reg, opcode &op)> arm_preop[] = {
    [] (psp_gpr_t psp_reg, opcode& op) -> void {    //PRE_OP_LSL_IMM
        //printf("PRE_OP_LSL_IMM %d \n", op.imm);
        loadReg(psp_reg, op.rs2);
        if(op.imm) emit_sll(psp_reg, psp_reg, op.imm);
    },
    [] (psp_gpr_t psp_reg, opcode& op) -> void {    //PRE_OP_LSL_REG
        //printf("PRE_OP_LSL_IMM %d \n", op.imm);
        loadReg(psp_t0, op.rs2>>8);
        emit_sltiu(psp_t1,psp_t0, 32);        
        loadReg(psp_reg, op.rs2&0xff);
        emit_sllv(psp_reg, psp_reg, psp_t0);
        emit_movz(psp_reg, psp_zero, psp_t1);

    },
    [] (psp_gpr_t psp_reg, opcode& op) -> void {   //PRE_OP_LSR_IMM
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        if(op.imm) {
            loadReg(psp_reg, op.rs2);
            emit_srl(psp_reg, psp_reg, op.imm);
        }
        else {
            emit_move(psp_reg, psp_zero);
        }
    },
    [] (psp_gpr_t psp_reg, opcode& op) -> void {   //PRE_OP_LSR_REG
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        loadReg(psp_t0, op.rs2>>8);
        emit_sltiu(psp_t1,psp_t0, 32);        
        loadReg(psp_reg, op.rs2&0xff);
        emit_srlv(psp_reg, psp_reg, psp_t0);
        emit_movz(psp_reg, psp_zero, psp_t1);
    },
    [] (psp_gpr_t psp_reg, opcode& op) -> void {   //PRE_OP_ASR_IMM
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        loadReg(psp_reg, op.rs2);
        emit_sra(psp_reg,psp_reg,op.imm ? op.imm : 31); 
    },
    [] (psp_gpr_t psp_reg, opcode& op) -> void {   //PRE_OP_ASR_REG
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        //TODO
    },
    [] (psp_gpr_t psp_reg, opcode& op) -> void {   //PRE_OP_ROR_IMM
        //TODO
    },

    [] (psp_gpr_t psp_reg, opcode& op) -> void {   //PRE_OP_NONE
        //TODO
    }
};

void emit_li(u32 reg,u32 data,u32 sz=0)
{
	if (is_u16(data) && sz!=2)
	{
		emit_ori(reg,psp_zero,data&0xFFFF);
	}
	else if (is_s16(data) && sz!=2)
	{
		emit_movi(reg, data&0xFFFF);
	}
	else
	{
		emit_lui(reg,data>>16);
		if ((sz==2) || (data&0xFFFF))
		{
			emit_ori(reg,reg,data&0xFFFF);
		}
	}
}

//load upper address
inline u32 emit_lua(u32 reg,u32 data)
{
    u32 hi = data>>16;
    u32 lo = data&0xFFFF;

    if (lo&0x8000){
        hi++;
        lo = data - (hi<<16);
    }

    emit_lui(reg,u16(hi));
	return lo;
}

uint32 emit_Halfbranch(int cond)
{
	static const uint8 cond_bit[] = {0x40, 0x40, 0x20, 0x20, 0x80, 0x80, 0x10, 0x10};

	if(cond < 8)
	{
      emit_andi(psp_t0, psp_gp, cond_bit[cond]);

      emit_nop();
      emit_nop();
      return emit_getPointAdr() - 8;
	}

   switch (cond){

      case 8:  
      case 9:
         emit_ext(psp_a1,psp_gp,6,5);
         emit_xori(psp_t0,psp_a1,0b01);

         emit_nop();
         emit_nop();
      break;

      case 10:
      case 11:

         emit_ext(psp_a1,psp_gp,7,7);
         emit_ext(psp_at,psp_gp,4,4);

         emit_xor(psp_t0,psp_a1,psp_at);

         emit_nop();
         emit_nop();

      break;

      case 12:
      case 13:

         emit_ext(psp_a1,psp_gp,7,6);
         emit_ext(psp_at,psp_gp,4,3);

         emit_andi(psp_at,psp_at,0b10);
         emit_xor(psp_t0,psp_a1,psp_at);

         emit_nop();
         emit_nop();
      break;

      default:
        return 0;
        //die("emit_Halfbranch: invalid cond\n"); 
   }

   return emit_getPointAdr() - 8;
}

void CompleteCondition(u32 cond, u32 _addr, u32 label){
    if(cond < 8)
	{
      if (cond&1)
         emit_bnelC(psp_t0,psp_zero,label,_addr);
      else
         emit_beqlC(psp_t0,psp_zero,label,_addr);
      return;
   }

   if (cond&1)
      emit_beqlC(psp_t0,psp_zero,label,_addr);
   else
      emit_bnelC(psp_t0,psp_zero,label,_addr);
}

void emit_prefetch(const u8 isize, bool saveR15, bool is_ITP){

   static int skip = 1;

   if (saveR15 || is_ITP){

    emit_addiu(psp_fp, psp_fp, isize * skip);

    if (is_ITP)
        emit_sw(psp_fp, psp_k0, _next_instr);

    emit_addiu(psp_at, psp_fp, isize);
    emit_sw(psp_at, psp_k0, _R15);

    skip = 1;
   }else
        skip++;
} 


INLINE void emit_bic(u32 dst,u32 a0, u32 a1)
{
    emit_not(a1, a1);
	emit_and(dst, a0, a1);
}

INLINE void emit_bici(u32 dst,u32 a0, u32 a1)
{
	emit_andi(dst, a0, ~a1);
}



#define gen_nativeOP(opType, n_op, n_op_imm, sign) \
template <bool imm, bool rev> void arm_##opType(opcode &op){ \
    loadReg(psp_a0, op.rs1); \
    if (imm){ \
        if (!rev && is_##sign##16(op.imm)) \
            emit_##n_op_imm(psp_v0, psp_a0, op.imm); \
        else{ \
            emit_li(psp_a1, op.imm); \
            if (!rev) \
                emit_##n_op(psp_v0, psp_a0, psp_a1); \
            else \
                emit_##n_op(psp_v0, psp_a1, psp_a0); \
        } \
    }else{ \
        arm_preop[op.preOpType](psp_a1 , op); \
        if (!rev) \
                emit_##n_op(psp_v0, psp_a0, psp_a1); \
        else \
            emit_##n_op(psp_v0, psp_a1, psp_a0); \
    } \
    storeReg(psp_v0, op.rd); \
    if (op.rd == 15) \
        emit_sw(psp_v0, RCPU, _next_instr); \
}

#define genThumb_nativeOP(opType, n_op, n_op_imm, sign) \
template <bool imm, bool rev> void thumb_##opType(opcode &op){ \
    loadReg(psp_a0, op.rs1); \
    if (imm){ \
        if (!rev && is_##sign##16(op.imm)) \
            emit_##n_op_imm(psp_v0, psp_a0, op.imm); \
        else{ \
            emit_li(psp_a1, op.imm); \
            if (!rev) \
                emit_##n_op(psp_v0, psp_a0, psp_a1); \
            else \
                emit_##n_op(psp_v0, psp_a1, psp_a0); \
        } \
    }else{ \
        arm_preop[op.preOpType](psp_a1 , op); \
        if (!rev) \
                emit_##n_op(psp_v0, psp_a0, psp_a1); \
        else \
            emit_##n_op(psp_v0, psp_a1, psp_a0); \
    } \
    storeReg(psp_v0, op.rd); \
    if (op.rd == 15) \
        emit_sw(psp_v0, RCPU, _next_instr); \
}

gen_nativeOP(and, and, andi, u);
gen_nativeOP(or, or, ori, u);
gen_nativeOP(xor, xor, xori, u);
gen_nativeOP(add, addu, addiu, s);
gen_nativeOP(sub, subu, subiu, s);
gen_nativeOP(bic, bic, bici, u);

//Do you want speed? call this function if you want to see some black magic :D

void block::optimize_basicblock(){
    opcode *prev_op = 0;


    //opcode &last_op = opcodes.back();


    //check if the block is a data invalidate block
    if (opcodes.size() == 4){
        if (opcodes[0]._op == OP_MOV && opcodes[1]._op == OP_MRC_MCR){
            printf("data invalidate block detected\n");

            //do just the last jump
            opcodes.erase(opcodes.begin(), opcodes.begin() + 3);

            printf("block size: %d\n", opcodes.size());
        }
    }



    for(opcode& op : opcodes){

        if (prev_op && prev_op->_op == OP_ITP && op.condition == -1 && op._op == OP_ITP) op.extra_flags = EXTFL_SKIPSAVEFLAG;
        if (prev_op && prev_op->_op == OP_ITP && prev_op->condition == -1) prev_op->extra_flags = EXTFL_SKIPLOADFLAG;

        // combine branches (slows down the emulator for some strange reason)
        /*if (prev_op && op.op_pc != opcodes.back().op_pc){
            //skip only the ops that doesn't change the flags
            if (( prev_op->_op <= OP_MVN && prev_op->_op > OP_ITP && op._op > OP_ITP && op._op <= OP_MVN)  && prev_op->condition == op.condition && op.condition != -1){
                if (prev_op->extra_flags & EXTFL_SAVECOND) prev_op->extra_flags ^= EXTFL_SAVECOND;
                //if (prev_op->extra_flags & EXTFL_RELOADPC) prev_op->extra_flags ^= EXTFL_RELOADPC;
                op.extra_flags |= EXTFL_MERGECOND;

                if ( prev_op && (prev_op->_op == OP_MOV || prev_op->_op == OP_MVN || prev_op->_op == OP_MOV_S || prev_op->_op == OP_MEMCPY) && 
                    (op._op >= OP_AND && op._op <= OP_SUB || (op._op >= OP_MOV && op._op <= OP_STRH)) && 
                    prev_op->rd == op.rs2 && (prev_op->preOpType == PRE_OP_LSL_IMM || prev_op->preOpType == PRE_OP_ASR_IMM) && prev_op->condition == op.condition) {

                    if (op.rd == prev_op->rd) prev_op->rd = -1;
                    op.rs2 = -1;
                }

                //op.extra_flags |= EXTFL_RELOADPC;
            }
        }*/

        //check for useless operation
        /*if (op.preOpType == PRE_OP_IMM && (op.imm == 0))
        {
            if ((op._op == OP_ORR || op._op == OP_EOR || op._op == OP_SUB)) {
                printf("Translated to move - OP: %x\n", op._op);
                op._op = OP_MOV;
                op.preOpType = PRE_OP_NONE;
            }else if (op._op == OP_AND) {
                printf("Translated AND to mov0\n");
                op._op = OP_MOV;
                op.imm = 0;
                op.preOpType = PRE_OP_IMM;
            }else if (op._op == OP_RSB) {
                //printf("Translated RSB to neg\n");
                //printf("rd : %d op.rs1: %d, op.rs2: %d\n", op.rd, op.rs1, op.rs2);
                op._op = OP_NEG; 
            } 
        }


        // optimize mul if we can
        /*if (prev_op && prev_op->_op == OP_MOV && prev_op->preOpType == PRE_OP_IMM && op._op == OP_MUL && prev_op->rd == op.rs2){
            //if (op.rs1 == op.rs2)
           //printf("optimized mul %d\n", prev_op->imm);

           op._op = OP_FAST_MUL;
           prev_op = &op;
          // continue;
        }*/
    #if 1
        if (prev_op && prev_op->_op == OP_LDRH && (op._op == OP_STRH /*|| op._op == OP_STR*/) && prev_op->rd == op.rs1 && op.extra_flags & EXTFL_DIRECTMEMACCESS && prev_op->extra_flags & EXTFL_DIRECTMEMACCESS){
            
            
            if (prev_op->preOpType == PRE_OP_IMM && op.preOpType == PRE_OP_IMM){
                prev_op->_op = OP_NOP;
                op._op = OP_MEMCPY;
                op.preOpType = op._op == OP_STR ? OP_32BIT : OP_16BIT;
                op.rs2 = prev_op->rs1;
                //printf("prev_op->imm: %d, op.imm: %d\n", prev_op->imm, op.imm);
                op.imm = prev_op->imm | (op.imm << 16);
                /*printf("prev_op->imm: %d, op.imm: %d\n", op.imm&0xFFFF, op.imm>>16);
                printf("______________\n");*/

            }

        }

        //severe speed gain with that !
        if ( prev_op && (prev_op->_op == OP_MOV || prev_op->_op == OP_MVN || prev_op->_op == OP_MOV_S || prev_op->_op == OP_MEMCPY) && 
             (op._op >= OP_AND && op._op <= OP_SUB || (op._op >= OP_MOV && op._op <= OP_STRH)) && 
             prev_op->rd == op.rs2 && (prev_op->preOpType == PRE_OP_LSL_IMM || prev_op->preOpType == PRE_OP_ASR_IMM) && prev_op->condition == op.condition) {

            if (op.rd == prev_op->rd) prev_op->rd = -1;
            op.rs2 = -1;
        }
        #endif

        prev_op = &op;
    } 

    /*if (noReadWriteOP && branch_addr == start_addr){
        for(opcode& op : opcodes){
            if (op._op == OP_SWI){
                idleLoop = true;
                printf("idle loop detected\n");
                break;
            }
        }
    }*/

}

void block::optimize_basicblockThumb(){
    opcode *prev_op = 0;
    opcode *prev_ITP = 0;

    for(opcode& op : opcodes){

        if (prev_ITP && op._op == OP_ITP) op.extra_flags |= EXTFL_SKIPSAVEFLAG;

        if (op._op == OP_ITP){
            prev_op = 0;
            prev_ITP = &op;
            continue;
        }

        prev_ITP = 0;
        prev_op = &op;
    }

    /*if (noReadWriteOP && branch_addr == start_addr){
        for(opcode& op : opcodes){
            if (op._op == OP_SWI){

                idleLoop = true;
                break;
            }
        }
    } */
}

extern "C" void set_sub_flags();
extern "C" void set_and_flags();
extern "C" void set_op_logic_flags();

#define cpu (&ARMPROC)

void lastBeforeCrash(int a0, int a1){
    printf("0x%x\n", a1);
}

u32 memRead(u32 addr){
    if((addr&(~0x3FFF)) == MMU.DTCMRegion) printf("DTCM read 0x%x, coglione\n", addr);
    return T1ReadLong_guaranteedAligned( MMU.MAIN_MEM, addr & _MMU_MAIN_MEM_MASK32);
}

void EmitReadFunction(u32 addr){
	unsigned 	o_ra;
	extern u8 *CodeCache;

	//Execute the patch at the end (overwrite ra addr inside the sp)
	asm volatile("addiu $2, $31, -8"); 
	asm volatile("sw $2, 0x14($29)");

	//Get the current ra 
	asm volatile("sw $31, %0":"=m"(o_ra));

	u32 _ptr = emit_Set((o_ra - 8) - (u32)&CodeCache);

    //printf("EmitReadFunction: 0x%x\n", o_ra);

	if((addr&(~0x3FFF)) == MMU.DTCMRegion){
        u32 dtcm_addr_arr = emit_lua(psp_t1, (u32)MMU.ARM9_DTCM);
        emit_andi(psp_t0, psp_a0, 0x3FFF);
        emit_addu(psp_t0, psp_t1, psp_t0);
        emit_lw(psp_v0, psp_t0, dtcm_addr_arr);
    }
    else{
        mem_off = emit_lua(psp_t1, (u32)MMU.MAIN_MEM);
        emit_ext(psp_t0, psp_a0, 21, 2);
        emit_sll(psp_t0, psp_t0, 2);
        emit_addu(psp_t0, psp_t1, psp_t0);
        emit_lw(psp_v0, psp_t0, mem_off);
    }

	emit_Set(_ptr);

	//make_address_range_executable(o_ra - 8, o_ra - 4);
	u32 addr_start = o_ra - 8;
	__builtin_allegrex_cache(0x1a, addr_start);
	__builtin_allegrex_cache(0x08, addr_start);
}

template<int PROCNUM>
static u16 FASTCALL _LDRH(u32 adr)
{
	return READ16(cpu->mem_if->data, adr);
} 

template<int PROCNUM>
static void FASTCALL _STRH(u32 regs, u32 imm)
{
    u32 adr = (u32)cpu->R[regs>>8] + imm;
    u32 data = (u32)cpu->R[regs&0xFF];
	WRITE16(cpu->mem_if->data, adr, data);
}


bool flag_loaded = false;
bool use_flags = false;
bool islast_op = false;

void load_flags(){
    if (use_flags && !flag_loaded)
    {
        emit_lbu(psp_gp, RCPU, _flags+3);
        flag_loaded = true;
    }
}

void store_flags(){
    if (use_flags && flag_loaded)
    {
        emit_sb(psp_gp, RCPU, _flags+3);
        flag_loaded = false;
    }
}

template<int PROCNUM>
void emitARMOP(opcode& op){
    switch(op._op){

        case OP_FAST_MUL:
        {

            //WARNING THIS OP WORKS ONLY IF THE PREV OP IS A MOVE OP WITH THE PSP REG A1 AS TEMP REG
            loadReg(psp_a0, op.rs1);
            // round to nearest 2 pow
            u32 near2 = 1;
            u32 bit = 0;
            while (near2 <= op.imm) {
                near2 <<= 1;
                ++bit;
            }
            bit--; near2 >>= 1;

            printf("near2: %d %d\n", near2, op.imm);

            u32 shift = op.imm - near2;
            break;
        }

        case OP_ITP:
        {
            ///if (!(op.extra_flags & EXTFL_SKIPSAVEFLAG)) 
            store_flags();

            emit_li(psp_a0, op.rs1); 

            uint32_t optmizeDelaySlot = emit_SlideDelay();

            emit_jal(arm_instructions_set[INSTRUCTION_INDEX(op.rs1)]);
            emit_Write32(optmizeDelaySlot);

            load_flags();

            //emit_addu(psp_k1, psp_k1, psp_v0);

            /*if (!(op.extra_flags & EXTFL_SKIPLOADFLAG) && !islast_op)
                load_flags();*/

            intr_instr++;
        }
        break;
        case OP_AND:
            if (op.preOpType == PRE_OP_IMM)
                arm_and<true, false>(op);
            else 
                arm_and<false, false>(op);
        break;

        case OP_BIC:
            if (op.preOpType == PRE_OP_IMM)
                arm_bic<true, false>(op);
            else 
                arm_bic<false, false>(op);
        break;

        case OP_ORR_S:
        case OP_ORR:
            if (op.preOpType == PRE_OP_IMM)
                arm_or<true, false>(op);
            else 
                arm_or<false, false>(op);

            if (op._op == OP_ORR_S){
                uint32_t optmizeDelaySlot = emit_SlideDelay();
                emit_jal(set_op_logic_flags);
                emit_Write32(optmizeDelaySlot);      
            }
        break;
        case OP_EOR_S:
        case OP_EOR:
            if (op.preOpType == PRE_OP_IMM)
                arm_xor<true, false>(op);
            else 
                arm_xor<false, false>(op);

            if (op._op == OP_EOR_S){
                uint32_t optmizeDelaySlot = emit_SlideDelay();
                emit_jal(set_op_logic_flags);
                emit_Write32(optmizeDelaySlot);     
            }
        break;

        case OP_ADD_S:
        case OP_ADD:
            if (op.preOpType == PRE_OP_IMM)
                arm_add<true, false>(op);
            else 
                arm_add<false, false>(op);
            
            if (op._op == OP_ADD_S){
                uint32_t optmizeDelaySlot = emit_SlideDelay();
                emit_jal(set_op_logic_flags);
                emit_Write32(optmizeDelaySlot);                   
            }
        break;
        case OP_SUB:
            if (op.preOpType == PRE_OP_IMM)
                arm_sub<true, false>(op);
            else 
                arm_sub<false, false>(op);
        break;

        case OP_STMIA:
        {
            //printf("ADDR: 0x%x\n", emit_getCurrAdr());

            loadReg(psp_a0, op.rs2);                    // load dst stack addr

            for(int j = 0, n = 4; j<16; j++)
                if(BIT_N(op.rs1, j))
                {

                    emit_jal(_MMU_write32<PROCNUM>); 
                    loadReg(psp_a1, j);                 // load reg val

                    loadReg(psp_a0, op.rs2);             // load dst stack addr
                    emit_addiu(psp_a0,psp_a0, n);        // add 4 to mem addr
                    n+= 4;
                }


                //remove the last 2 useless instructions
                emit_Skip(-8);
        }
        break;

        case OP_RSB:
            if (op.preOpType == PRE_OP_IMM)
                arm_sub<true, true>(op);
            else 
                arm_sub<false, true>(op);
        break;
        case OP_MUL:
            loadReg(psp_a0, op.rs1);
            
            if (op.rs2 != -1)
                loadReg(psp_a1, op.rs2);
            else
            {
                emit_li(psp_a1, op.imm);
            }

            emit_mult(psp_a0, psp_a1);
            emit_mflo(psp_v0);
            storeReg(psp_v0, op.rd);
        break; 

        case OP_MLA:
            //printf("ADDR: 0x%x\n", emit_getCurrAdr());

            loadReg(psp_a0, op.rs1);
            loadReg(psp_a1, op.rs2);
            loadReg(psp_a2, op.imm);

            emit_mult(psp_a0, psp_a1);
            emit_mflo(psp_v0);
            emit_addu(psp_v0, psp_v0, psp_a2);

            /*emit_mtlo(psp_a2);
            emit_maddu(psp_a0, psp_a1);
            emit_mflo(psp_v0);*/

            storeReg(psp_v0, op.rd);
        break; 

        case OP_CLZ:
            loadReg(psp_a0, op.rs1);
            emit_clz(psp_v0, psp_a0);
            storeReg(psp_v0, op.rd);
        break;

        case OP_NEG:
            loadReg(psp_a0, op.rs1);
            emit_negu(psp_v0, psp_a0);
            storeReg(psp_v0, op.rd);
        break;

        case OP_STRH:
        case OP_STR:
        {
           
           loadReg(psp_a0, op.rd);
           loadReg(psp_a1, op.rs1);

           if (op.imm != 0){

                if (op.preOpType == PRE_OP_IMM)   emit_addiu(psp_a0, psp_a0, op.imm);
                else {
                    if (op.preOpType == PRE_OP_PRE_P)   {
                        emit_addiu(psp_a0, psp_a0, op.imm);
                        storeReg(psp_a0, op.rd);
                    }
                    else if (op.preOpType == PRE_OP_PRE_M) {
                        emit_addiu(psp_a0, psp_a0, -op.imm);
                        storeReg(psp_a0, op.rd);
                    }

                    // what if we do it here ?
                    else if (op.preOpType == PRE_OP_POST_P)   {
                        emit_addiu(psp_t0, psp_a0, op.imm);
                        storeReg(psp_t0, op.rd);
                    }
                    else if (op.preOpType == PRE_OP_POST_M) {
                        emit_addiu(psp_t0, psp_a0, -op.imm);
                        storeReg(psp_t0, op.rd);
                    }
                }
           }else{

                if (op.preOpType == PRE_OP_REG_OFF){
                    loadReg(psp_a3, op.rs2);
                    if (op.rs2 == -1) emit_addu(psp_a0, psp_a0, psp_a1);
                    else              emit_addu(psp_a0, psp_a0, psp_a3);
                }
                else if (op.preOpType == PRE_OP_M_REG_OFF){
                    loadReg(psp_a3, op.rs2);
                    if (op.rs2 == -1) emit_subu(psp_a0, psp_a0, psp_a1);
                    else              emit_subu(psp_a0, psp_a0, psp_a3);
                }
           }

            if (op._op == OP_STR) {
                /*if (op.extra_flags & EXTFL_DIRECTMEMACCESS){
                        //printf("STR ADDR: 0x%x\n", emit_getCurrAdr());
                        u32 dtcm_addr = emit_lua(psp_t3, (u32)&MMU.DTCMRegion);
                        emit_lw(psp_t1, psp_t3, dtcm_addr);

                        emit_andi(psp_t2, psp_a0, 0x3FFF);
                        emit_xor(psp_t0, psp_a0, psp_t2);
                        emit_bnel(psp_t0, psp_t1, (emit_getCurrAdr() + 4 * 5));
                        emit_ext(psp_t0, psp_a0, 21, 2);;

                        {
                            emit_addu(psp_t0, psp_t3, psp_t2);
                            emit_j((emit_getCurrAdr() + 4 * 10));
                            emit_sw(psp_a1, psp_t0, dtcm_addr - offsetBetween(MMU.DTCMRegion, MMU.ARM9_DTCM));
                        }
                        //else
                        {
                            emit_sll(psp_t0, psp_t0, 2);
                            emit_addu(psp_t1, psp_t3, psp_t0);
                            emit_sw(psp_a1, psp_t1, (dtcm_addr + offsetBetween2(MMU.DTCMRegion, MMU.MAIN_MEM)))
                            //invalidate jit cache
                            u32 jit_bank = emit_lua(psp_t1, (u32)JIT.MAIN_MEM);
                            emit_srl(psp_t0, psp_t0, 1);
                            emit_addu(psp_t1, psp_t1, psp_t0);
                            emit_sw(psp_zero, psp_t1, jit_bank);
                        }
                }else*/{
                    emit_jal(_MMU_write32<PROCNUM>); 
                    emit_ins(psp_a0, psp_zero, 1, 0);
                }
            }else if (op._op == OP_STRH) {
                if (op.extra_flags & EXTFL_DIRECTMEMACCESS){
                        //printf("ADDR: 0x%x\n", emit_getCurrAdr());
                        u32 dtcm_addr = emit_lua(psp_t3, (u32)&MMU.DTCMRegion);
                        emit_lw(psp_t1, psp_t3, dtcm_addr);

                        emit_move(psp_t0, psp_a0);
                        emit_ins(psp_t0, psp_zero, 13, 0);
                        emit_bnel(psp_t0, psp_t1, (emit_getCurrAdr() + 4 * 6));
                        emit_ext(psp_t0, psp_a0, 21, 1);
                        {
                            emit_andi(psp_t0, psp_a0, 0x3FFE);
                            emit_addu(psp_t0, psp_t3, psp_t0);
                            emit_j((emit_getCurrAdr() + 4 * 9));
                            emit_sh(psp_a1, psp_t0, dtcm_addr - offsetBetween(MMU.DTCMRegion, MMU.ARM9_DTCM));
                        }
                        //else
                        {
                            emit_sll(psp_t1, psp_t0, 1);
                            emit_addu(psp_t1, psp_t3, psp_t1);
                            emit_sh(psp_a1, psp_t1, (dtcm_addr + offsetBetween2(MMU.DTCMRegion, MMU.MAIN_MEM)));

                            //invalidate jit cache
                            u32 jit_bank = emit_lua(psp_t1, (u32)JIT.MAIN_MEM);
                            emit_sll(psp_t0, psp_t0, 2);
                            emit_addu(psp_t1, psp_t1, psp_t0);
                            emit_sw(psp_zero, psp_t1, jit_bank);
                        }
                }else{
                    emit_jal(_MMU_write16<PROCNUM>); 
                    emit_ins(psp_a0, psp_zero, 0, 0);
                }
            }

           break;
        }

        case OP_LDRH:
        case OP_LDR:
        {
           loadReg(psp_a0, op.rs1);

            if (op.imm != 0){

                if (op.preOpType == PRE_OP_IMM)   emit_addiu(psp_a0, psp_a0, op.imm);
                else {
                    if (op.preOpType == PRE_OP_PRE_P)   {
                        emit_addiu(psp_a0, psp_a0, op.imm);
                        storeReg(psp_a0, op.rs1);
                    }
                    else if (op.preOpType == PRE_OP_PRE_M) {
                        emit_addiu(psp_a0, psp_a0, -op.imm);
                        storeReg(psp_a0, op.rs1);
                    }

                    // what if we do it here ?
                    else if (op.preOpType == PRE_OP_POST_P)   {
                        emit_addiu(psp_t0, psp_a0, op.imm);
                        storeReg(psp_t0, op.rs1);  
                    }
                    else if (op.preOpType == PRE_OP_POST_M) {
                        emit_addiu(psp_t0, psp_a0, -op.imm);
                        storeReg(psp_t0, op.rs1);
                    }
                }
           }else{

                if (op.preOpType == PRE_OP_REG_OFF){
                    loadReg(psp_a3, op.rs2);
                    if (op.rs2 == -1) emit_addu(psp_a0, psp_a0, psp_a1);
                    else              emit_addu(psp_a0, psp_a0, psp_a3);
                }
                else if (op.preOpType == PRE_OP_M_REG_OFF){
                    loadReg(psp_a3, op.rs2);
                    if (op.rs2 == -1) emit_subu(psp_a0, psp_a0, psp_a1);
                    else              emit_subu(psp_a0, psp_a0, psp_a3);
                }
           }
        
            
            if (op._op == OP_LDR) {

                if (op.extra_flags & EXTFL_DIRECTMEMACCESS){
                    //printf("ADDR: 0x%x\n", emit_getCurrAdr());

                    u32 dtcm_addr = emit_lua(psp_t3, (u32)&MMU.DTCMRegion);

                    emit_lw(psp_t1, psp_t3, dtcm_addr);

                    emit_andi(psp_t2, psp_a0, 0x3FFF);
                    emit_xor(psp_t0, psp_a0, psp_t2);
                    emit_bnel(psp_t0, psp_t1, (emit_getCurrAdr() + 4 * 5));
                    emit_ext(psp_t0, psp_a0, 21, 2);
                    {
                        emit_addu(psp_t0, psp_t3, psp_t2);
                        emit_j((emit_getCurrAdr() + 4 * 5));
                        emit_lw(psp_v0, psp_t0, dtcm_addr - offsetBetween(MMU.DTCMRegion, MMU.ARM9_DTCM));
                    }
                    //else
                    {
                        emit_sll(psp_t0, psp_t0, 2);
                        emit_addu(psp_t0, psp_t3, psp_t0);
                        emit_lw(psp_v0, psp_t0, (dtcm_addr + offsetBetween2(MMU.DTCMRegion, MMU.MAIN_MEM)));
                    }


                }else{
                    emit_jal(_MMU_read32<PROCNUM>);
                    emit_ins(psp_a0, psp_zero, 1, 0);
                }
            }else if (op._op == OP_LDRH) {
                if (op.extra_flags & EXTFL_DIRECTMEMACCESS){

                    u32 dtcm_addr = emit_lua(psp_t3, (u32)&MMU.DTCMRegion);
                    emit_lw(psp_t1, psp_t3, dtcm_addr);

                    emit_move(psp_t0, psp_a0);
                    emit_ins(psp_t0, psp_zero, 13, 0);
                    emit_bnel(psp_t0, psp_t1, (emit_getCurrAdr() + 4 * 6));
                    emit_ext(psp_t0, psp_a0, 21, 1);
                    {
                        emit_andi(psp_t0, psp_a0, 0x3FFE);
                        emit_addu(psp_t0, psp_t3, psp_t0);
                        emit_j((emit_getCurrAdr() + 4 * 5));
                        emit_lhu(psp_v0, psp_t0, dtcm_addr - offsetBetween(MMU.DTCMRegion, MMU.ARM9_DTCM));
                    }
                    //else
                    {
                        emit_sll(psp_t0, psp_t0, 1);
                        emit_addu(psp_t0, psp_t3, psp_t0);
                        emit_lhu(psp_v0, psp_t0, (dtcm_addr + offsetBetween2(MMU.DTCMRegion, MMU.MAIN_MEM)));
                    }
                }else
                {
                    emit_jal(_MMU_read16<PROCNUM>);
                    emit_ins(psp_a0, psp_zero, 0, 0);
                }
            }

            storeReg(psp_v0, op.rd);
           break;
        }

        case OP_SUB_S:
        case OP_CMP:
        {
            if (op.preOpType == PRE_OP_IMM){
                arm_sub<true, false>(op);
                if (is_u16(op.imm))
                    emit_li(psp_a1, op.imm);
            }else
                arm_sub<false, false>(op);

            //printf("0x%x\n", emit_getCurrAdr());

            uint32_t optmizeDelaySlot = emit_SlideDelay();
            emit_jal(set_sub_flags);
            emit_Write32(optmizeDelaySlot); 
            
            break;
        }

        case OP_AND_S:
        case OP_TST:
        {
            if (op.preOpType == PRE_OP_IMM){
                arm_and<true, false>(op);
                emit_li(psp_a1, op.imm);
             }else 
                arm_and<false, false>(op);
            
            uint32_t optmizeDelaySlot = emit_SlideDelay();
            emit_jal(set_and_flags);
            emit_Write32(optmizeDelaySlot);  

            break;
        }

        case OP_SWI:
        {
           emit_jal(cpu->swi_tab[op.rs1]); 
           emit_nop();
        }

        case OP_MOV:
        case OP_MVN:
        case OP_MVN_S:
        case OP_MOV_S:


            if (op.imm == 0 && op.preOpType == PRE_OP_IMM){
                storeReg(psp_zero, op.rd);
                
            //is that valid also for mvn_s? not sure
                if (op._op == OP_MOV_S || op._op == OP_MVN_S)
                    emit_ori(psp_gp, psp_gp, 0x40);     
                
                break;
            }

            if (op.preOpType == PRE_OP_IMM){
                emit_li(psp_a1, op.imm);
            }else{
                if (op.preOpType == PRE_OP_NONE){
                    loadReg(psp_a1, op.rs1);
                }else
                    arm_preop[op.preOpType](psp_a1, op);

                if (op._op == OP_MVN_S || op._op == OP_MVN)
                    emit_not(psp_a1, psp_a1);
            }

            storeReg(psp_a1, op.rd);

            if (op.rd == 15)
                emit_sw(psp_a1, RCPU, _next_instr);
            
            if (op._op == OP_MOV_S || op._op == OP_MVN_S){
                emit_jal(set_op_logic_flags);
                emit_move(psp_v0, psp_a1);     
            }
        break;

        case OP_MEMCPY:
        {

            u32 ldr_imm = op.imm & 0xFFFF;
            u32 str_imm = (op.imm >> 16) & 0xFFFF;

            //printf("rd: %d, rs1: %d, rs2: %d, imm: %d\n", op.rd, op.rs1, op.rs2, op.imm);
            //printf("ADDR: 0x%x\n", emit_getCurrAdr());

            loadReg(psp_a0, op.rs2);
            if (ldr_imm != 0) 
                emit_addiu(psp_a0, psp_a0, ldr_imm);

            
            emit_jal(_MMU_read16<PROCNUM>);
            emit_ins(psp_a0, psp_zero, 0, 0);

            emit_move(psp_a1, psp_v0);
            
            storeReg(psp_v0, op.rs1);
            loadReg(psp_a0, op.rd);

            if (str_imm != 0) 
                emit_addiu(psp_a0, psp_a0, str_imm);

            if (op.preOpType == OP_16BIT){
                emit_jal(_MMU_write16<PROCNUM>); 
                emit_ins(psp_a0, psp_zero, 0, 0);
            }else if (op.preOpType == OP_32BIT) {
                emit_jal(_MMU_write32<PROCNUM>); 
                emit_ins(psp_a0, psp_zero, 1, 0);
            }

        }
        break;

        case OP_BXC:
        {
            //printf("0x%x\n", emit_getCurrAdr());

            // u32 tmp = cpu->R[REG_POS(i, 0)];
            //loadReg(psp_a0, op.rs1);

            // BIT0(tmp);
            //emit_ext(psp_t0, psp_a0, 0, 1);

            // cpu->CPSR.bits.T = BIT0(tmp);
            /*emit_lb(psp_t1, RCPU, _flags+1);
            emit_ins(psp_t0, psp_t1, 5, 6);
            emit_sb(psp_t0, RCPU, _flags+1);*/

            emit_li(psp_a0, op.imm);
            emit_addu(psp_fp, psp_fp, psp_a0);
            
            // cpu->R[15] = tmp & (0xFFFFFFFC|(cpu->CPSR.bits.T<<1));
            /*emit_ext(psp_fp, psp_a0, 1, 31);
            emit_sll(psp_fp, psp_fp, 1);*/
            emit_ins(psp_fp, psp_zero, 1, 0);
            //emit_srlv(psp_fp, psp_fp, psp_t0);

            emit_sw(psp_fp, psp_k0, _R15);
            emit_sw(psp_fp, psp_k0, _next_instr);
            emit_sw(psp_fp, psp_k0, _instr_adr);
        }
        break;
    }
}

template<int PROCNUM> 
void emitThumbOP(opcode& op){
    switch(op._op) {
        case OP_ITP:
        {
            if (!(op.extra_flags & EXTFL_SKIPSAVEFLAG)) 
                store_flags();


            reg_alloc.dealloc_all();  

            emit_jal(thumb_instructions_set[op.rs1 >> 6]);

            emit_movi(psp_a0,op.rs1&0xFFFF); 

            intr_instr++;
        }
        break;

        case OP_LSR_0:
        {
            //printf("0x%x\n", emit_getCurrAdr());
            psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);

            if (dst == psp_v0) storeReg(psp_zero, op.rd); else emit_move(dst, psp_zero);

            emit_sra(psp_v0, rs1, 31);
            emit_sll(psp_v0, psp_v0, 2);
            emit_ori(psp_gp, psp_gp, 2);
            emit_ins(psp_gp, psp_v0, _flag_C8, 3);
        }
        break;

        //TODO ADD reg_alloc.end at the end of the opcde to store the temp reg (in case the static one are full)
        case OP_AND:
        {
            psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);

            emit_and(dst, rs1, rs2);
            emit_jal(set_op_logic_flags);
            if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(psp_v0, dst);                    
        }
        break;

        //TODO! check flag for sub/cmp - not all cases are correct!!!! for now seems to work but this must be 
        //                           one of the first thing to be analyzed if some games behaves wrongly!!
        case OP_SUB:
        {
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t dst = (op.rs1 == op.rd) ? rs1 : reg_alloc.getReg(op.rd, psp_v0, false); 
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1); 

            if (op.preOpType == PRE_OP_IMM){
                if (op.imm){
                    emit_li(rs2, op.imm);
                    emit_subu(dst, rs1, rs2);
                }else
                {
                    emit_move(dst, rs1);
                    emit_move(rs2, psp_zero);
                }
            }else{
                emit_subu(dst, rs1, rs2);
            }

            if (dst == psp_v0)  storeReg(psp_v0, op.rd);

            storeReg(dst, op.rd);
            emit_jal(set_sub_flags);
            if (dst != psp_v0)  emit_move(psp_v0, dst); 
            else emit_nop(); 
        }
        break;

        case OP_ADD:
        {
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t dst = (op.rs1 == op.rd) ? rs1 : reg_alloc.getReg(op.rd, psp_v0, false); 
            
            if (op.preOpType == PRE_OP_IMM){
                emit_addiu(dst, rs1, op.imm);
            }else{
                psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);
                emit_addu(dst, rs1, rs2);
            }
          
            if (!op.extra_flags&EXTFL_NOFLAGS){
                storeReg(dst, op.rd);
                emit_jal(set_and_flags);
                if (dst != psp_v0)  emit_move(psp_v0, dst); 
                else emit_nop(); 
            } 
            else if (dst == psp_v0)  storeReg(psp_v0, op.rd);
        }
        break;

        case OP_EOR:
        {
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t dst = (op.rs1 == op.rd) ? rs1 : reg_alloc.getReg(op.rd, psp_v0, false); 
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);

            emit_xor(dst, rs1, rs2);
            
            emit_jal(set_op_logic_flags);
            if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(psp_v0, dst);            
        }
        break;

        case OP_ORR:
        {
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t dst = (op.rs1 == op.rd) ? rs1 : reg_alloc.getReg(op.rd, psp_v0, false); 
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);

            emit_or(dst, rs1, rs2);
            
            emit_jal(set_op_logic_flags);
            if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(psp_v0, dst);          
        }
        break;

        case OP_TST:
        {
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);

            emit_jal(set_op_logic_flags);
            emit_and(psp_v0, rs1, rs2);          
        }
        break;

        case OP_SWI:
        {
            reg_alloc.dealloc(0);
            reg_alloc.dealloc(1);
            reg_alloc.dealloc(2);
            reg_alloc.dealloc(3);
            reg_alloc.dealloc(15);

            uint32_t optmizeDelaySlot = emit_SlideDelay();

            emit_jal(cpu->swi_tab[op.rs1]);
            emit_Write32(optmizeDelaySlot);
        }

        case OP_CMP:
        {
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);

            if (op.preOpType == PRE_OP_IMM)
                emit_li(psp_a1, op.imm);
            else if (rs2 != psp_a1) emit_move(psp_a1, rs2);
            
            if (rs1 != psp_a0) emit_move(psp_a0, rs1);
            

            emit_jal(set_sub_flags); 
            emit_subu(psp_v0, psp_a0, psp_a1);        
        }
        break;
 
        case OP_MUL:
        {
            psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);

            emit_mult(rs1, rs2);
            emit_mflo(dst);
            
            emit_jal(set_op_logic_flags);
            if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(psp_v0, dst);            
        }
        break;

        case OP_MOV:
        {
            psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);

            if (op.preOpType == PRE_OP_IMM){

                if (op.imm == 0){
                    if (dst == psp_v0)  storeReg(psp_zero, op.rd); else emit_move(dst, psp_zero); 
                    emit_ori(psp_gp, psp_gp, 0x40); 
                    break;
                }
                
                emit_li(dst, op.imm);
                emit_jal(set_op_logic_flags);
                if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(psp_v0, dst);
            }else{
                psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_v0);

                if (dst != psp_v0) emit_move(dst, rs1);

                if (op.rd == 15) emit_sw(dst, psp_k0, _next_instr);
                else{
                    emit_jal(set_op_logic_flags);
                    if (dst == psp_v0)  storeReg(rs1, op.rd); else emit_move(psp_v0, dst); 
                }
            }
        }
        break;

        case OP_LDR:
        case OP_LDRH:
        {
           psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
           psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);

           //printf("0x%x\n", emit_getCurrAdr());
 
            if (op.preOpType == PRE_OP_REG_OFF) {
                psp_gpr_t rs2 = reg_alloc.getReg(op.rs2, psp_a1);
                emit_addu(psp_a0, rs1, rs2);
            }
            else if (rs1 != psp_a0 || op.imm != 0)
                emit_addiu(psp_a0, rs1, op.imm);

            if (op._op == OP_LDR){
                emit_jal(_MMU_read32<PROCNUM>);
                emit_ins(psp_a0, psp_zero, 1, 0);
            }else{
                if (op.extra_flags & EXTFL_DIRECTMEMACCESS){

                    u32 dtcm_addr = emit_lua(psp_t3, (u32)&MMU.DTCMRegion);
                    emit_lw(psp_t1, psp_t3, dtcm_addr);

                    emit_move(psp_t0, psp_a0);
                    emit_ins(psp_t0, psp_zero, 13, 0);
                    emit_bnel(psp_t0, psp_t1, (emit_getCurrAdr() + 4 * 6));
                    emit_ins(psp_a0, psp_zero, 0, 0);
                    {
                        emit_andi(psp_t0, psp_a0, 0x3FFE);
                        emit_addu(psp_t0, psp_t3, psp_t0);
                        emit_j((emit_getCurrAdr() + 4 * 4));
                        emit_lhu(dst, psp_t0, dtcm_addr - offsetBetween(MMU.DTCMRegion, MMU.ARM9_DTCM));
                    }
                    //else
                    {
                        emit_addu(psp_t0, psp_t3, psp_t0);
                        emit_lhu(dst, psp_t0, (dtcm_addr + offsetBetween2(MMU.DTCMRegion, MMU.MAIN_MEM)));
                    }
                }else
                {
                    emit_jal(_MMU_read16<PROCNUM>);
                    emit_ins(psp_a0, psp_zero, 0, 0);
                }
            }

            if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(dst, psp_v0);
           break;
        }

        case OP_STR:
        case OP_STRH:
        {

           //printf("0x%x\n", emit_getCurrAdr());

           psp_gpr_t rs1 = reg_alloc.getReg(op.rd, psp_a0);
           psp_gpr_t rs2 = reg_alloc.getReg(op.rs1, psp_a1);


           if (op.preOpType == PRE_OP_REG_OFF) {
                //printf("0x%x\n", emit_getCurrAdr());
                psp_gpr_t rs3 = reg_alloc.getReg(op.rs2, psp_a2);
                emit_addu(psp_a0, rs1, rs3);
            }
            else if ((rs1 != psp_a0 || op.imm != 0))
                emit_addiu(psp_a0, rs1, op.imm);

           /*if (op.extra_flags & EXTFL_DIRECTMEMACCESS && op._op != OP_STR){

                if (op._op == OP_STR){
                    /*emit_ext(psp_t0, psp_a0, 21, 2);
                    emit_sll(psp_t0, psp_t0, 2);
                    emit_addu(psp_t0, psp_t1, psp_t0);
                    emit_sw(rs2, psp_t0, off);
                }else {
                    u32 dtcm_addr = emit_lua(psp_t3, (u32)&MMU.DTCMRegion);
                    emit_lw(psp_t1, psp_t3, dtcm_addr);

                    emit_move(psp_t0, psp_a0);
                    emit_ins(psp_t0, psp_zero, 13, 0);
                    emit_bnel(psp_t0, psp_t1, (emit_getCurrAdr() + 4 * 6));
                    emit_ext(psp_t0, psp_a0, 21, 1);
                    {
                        emit_andi(psp_t0, psp_a0, 0x3FFE);
                        emit_addu(psp_t0, psp_t3, psp_t0);
                        emit_j((emit_getCurrAdr() + 4 * 9));
                        emit_sh(rs1, psp_t0, dtcm_addr - offsetBetween(MMU.DTCMRegion, MMU.ARM9_DTCM));
                    }
                    //else
                    {
                        emit_sll(psp_t1, psp_t0, 1);
                        emit_addu(psp_t1, psp_t3, psp_t1);
                        emit_sh(rs1, psp_t1, (dtcm_addr + offsetBetween2(MMU.DTCMRegion, MMU.MAIN_MEM)));

                        //invalidate jit cache
                        u32 jit_bank = emit_lua(psp_t1, (u32)JIT.MAIN_MEM);
                        emit_sll(psp_t0, psp_t0, 2);
                        emit_addu(psp_t1, psp_t1, psp_t0);
                        emit_sw(psp_zero, psp_t1, jit_bank);
                    }
                }

           }else */{

                if (rs2 != psp_a1) emit_move(psp_a1, rs2);

                if (op._op == OP_STR){
                        emit_jal(_MMU_write32<PROCNUM>); 
                        emit_ins(psp_a0, psp_zero, 1, 0);
                }else{
                        emit_jal(_MMU_write16<PROCNUM>); 
                        emit_ins(psp_a0, psp_zero, 0, 0);
                }
            }

           break;
        }

        case OP_MVN:
        {
            psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);

            emit_not(dst, rs1);
            
            emit_jal(set_op_logic_flags);
            if (dst == psp_v0)  storeReg(psp_v0, op.rd); else emit_move(psp_v0, dst);       
        }
        break;



        default:
            printf("Unknown Thumb OP: %d\n", op._op);
            exit(1);
    }
}

void print_current_block(uint32_t pc){
    printf("Curr BLOCK: 0x%X\n", pc);
}

//Already compiled and optimized block
std::function<bool(bool)> arm_compiledOP[] = {
    [] (bool PROCNUM) -> bool { 
        emit_lw(psp_a0, psp_k0, 0x18);
        emit_andi(psp_a0, psp_a0, 0x3c);
        emit_lui(psp_a1, 0x620);
        emit_sll(psp_a0, psp_a0, 0xc);
        emit_addu(psp_a0, psp_a0, psp_a1);
        emit_sw(psp_a0, psp_k0, 0x10);

        currentBlock.emitArmBranch<0>();

        return true;
    }, 
    [] (bool PROCNUM) -> bool { 


        emit_jal(_LDRH<0>);

        emit_lw(psp_a0, psp_k0, 0x38);

        emit_andi(psp_v0, psp_v0, 0x4000);

        emit_srl(psp_t0, psp_v0, 31);
        emit_ins(psp_gp, psp_t0, _flag_N8, _flag_N8);

        emit_slti(psp_t0, psp_v0, 0x4000);
        emit_ins(psp_gp, psp_t0, _flag_V8, _flag_V8);

        emit_sltiu(psp_t0, psp_v0, 1);
        emit_ins(psp_gp, psp_t0, _flag_Z8, _flag_Z8);

        emit_bnel(psp_t0, psp_zero, emit_getCurrAdr() + 44);
        {
            emit_sw(psp_v0, psp_k0, 0x10);  //emit_nop();

            emit_jal(_LDRH<0>);

            emit_lw(psp_a0, psp_k0, 0x38);

            emit_ori(psp_v0, psp_v0, 0xc000);

            emit_sw(psp_v0, psp_k0, 0x10); 

            emit_movi(psp_a0, 0xa00);
            
           emit_jal(_STRH<0>);

            emit_move(psp_a1, psp_zero);

            emit_lw(psp_a0, psp_k0, 0x34);
            emit_sw(psp_a0, psp_k0, 0x14);
        }

        currentBlock.emitArmBranch<0>();

        return false;
    }
};


template<int PROCNUM>
bool block::emitThumbBlock(){

    use_flags = true;

    //printf("0x%X\n", emit_getCurrAdr()); 

   /* printf("---------------------------------\n");

    printf("BLOCK %s INFO: N ops: %d \n", block_hash, opcodes.size());

    for (int i = 1; i <= 16; ++i){
        printf("R%d: used %d times\n", i-1, reg_usage[i]);
    }*/

    char dasmbuf[1024] = {0};


    reg_alloc.reset(); 

    emit_li(psp_fp, opcodes.front().op_pc);

    opcode last_op = opcodes.back();
    const bool islastITP = last_op._op == OP_ITP;

    for(opcode op : opcodes){

        const u8 isize = /*(op._op == OP_NOP && op.rd == 0) ? 4 :*/ 2;

        if (op._op != OP_ITP) load_flags();

        reg_alloc.alloc_regs(op); 

        if (last_op.op_pc != op.op_pc)
            emit_prefetch(isize, op.rs1 == 15 || op.rs2 == 15, op._op == OP_ITP);
        else
            //Last op has always to save 
            emit_prefetch(isize, true, true);
        
        emitThumbOP<PROCNUM>(op);
    } 
     
    store_flags();
 
    reg_alloc.dealloc_all(); 

    //possible idle loop, do more checks here
    return idleLoop;

}

static bool instr_is_conditional(u32 opcode)
{
	return !(CONDITION(opcode) == 0xE
	         || (CONDITION(opcode) == 0xF && CODE(opcode) == 5));
}

template<int PROCNUM>
bool block::emitArmBlock(){

    uint32_t conditional_label = 0;
    uint32_t jump_sz = 0;

    intr_instr = 0;

    use_flags = uses_flags;

    char * found = strstr(compiled_functions_hash, block_hash);


    if (found != NULL) {
        return arm_compiledOP[(found - compiled_functions_hash) / 51](PROCNUM);
    }

   start_block = emit_getCurrAdr();
   emit_li(psp_fp, opcodes.front().op_pc);
    

    opcode last_op = opcodes.back();
    const bool islastITP = last_op._op == OP_ITP;


    // find OP_MCR_MRC and delete it
    for (auto it = opcodes.begin(); it != opcodes.end(); ++it){
        if (it->_op == OP_MRC_MCR){
            opcodes.erase(it);
            break;
        }
    }
    bool last = false; 
    load_flags();
    for(opcode op : opcodes){ 
        bool isConditional = op.condition == 0xF && instr_is_conditional(_MMU_read32<ARM9>(op.op_pc));
 
        //TODO: with nop and skip prefetch, the next_istr and r15 gets a wrong value
        const u8 isize = /*(op._op == OP_NOP && op.rd == 0) ? 8 :*/ 4;

        if (last_op.op_pc != op.op_pc && !isConditional)
            emit_prefetch(isize, op.rs1 == 15 || op.rs2 == 15, op._op == OP_ITP);
        else {
            //if (!(op._op == OP_BXC && op.condition < 14))
            //Last op has always to save 
            emit_prefetch(isize, true, true);
            last = true;
        }

        /*if (op.condition != -1 || (op._op >= OP_CMP))
            load_flags();*/


        //if (op.extra_flags & EXTFL_MERGECOND) printf("MERGECOND at 0x%08X\n", emit_getCurrAdr());
        
        if (isConditional) { 
            conditional(
                if (!last)
                    emit_prefetch(isize, op.rs1 == 15 || op.rs2 == 15, op._op == OP_ITP); 
                emitARMOP<PROCNUM>(op)
                )
        }else{
            conditional(emitARMOP<PROCNUM>(op))
        }

    }

    store_flags();

    //possible idle loop, do more checks here
    return idleLoop; 
}


template<int PROCNUM>
void block::emitArmBranch(){
    uint32_t conditional_label = 0;
    uint32_t jump_sz = 0;

    opcode op = opcodes.back();

    emit_li(psp_fp, op.op_pc + 4);
    emit_sw(psp_fp, psp_k0, _next_instr);

    emit_addiu(psp_at, psp_fp, 4);
    emit_sw(psp_at, psp_k0, _R15);

    conditional(emitARMOP<PROCNUM>(op))
}

// define the template 
template bool block::emitArmBlock<0>();
template bool block::emitArmBlock<1>();

template void block::emitArmBranch<0>();
template void block::emitArmBranch<1>();

template bool block::emitThumbBlock<0>();
template bool block::emitThumbBlock<1>();
