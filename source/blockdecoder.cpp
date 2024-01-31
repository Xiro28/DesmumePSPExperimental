#include "blockdecoder.h"

#include "mips_code_emiter.h"
#include "PSP/emit/psp_emit.h"
#include "armcpu.h"


#include "Disassembler.h"

#include <functional>

bool instr_does_prefetch(u32 opcode);

inline void loadReg(psp_gpr_t psp_reg, s32 nds_reg) { if (nds_reg != -1) emit_lw(psp_reg, RCPU, _reg(nds_reg)); }
inline void storeReg(psp_gpr_t psp_reg, s32 nds_reg) { if (nds_reg != -1) emit_sw(psp_reg, RCPU, _reg(nds_reg)); }

#include "psp_sra.h"

#define offsetBetween(x, x2) (((u32)&x) - ((u32)&x2[0]))
#define offsetBetween2(x, x2) (((u32)&x2[0]) - ((u32)&x))

u32 mem_off = 0;
u32 dtcm_addr_arr = 0;
int intr_instr = 0;

u32 start_block = 0;

block currentBlock;  
reg_allocation reg_alloc;
register_manager regman;

const char * compiled_functions_hash = 
    ">:1:05:8FDA19B2:C2AF4F28:998B99C6:F0596DB3:2FD2AA9B" //used by yoshi island ingame
    ">:1:07:88DD1AED:096B3BDA:7857790C:6D443C78:A4ADDB58" //a lot used in pokemon diamond 
    ;

//make an array of pointers to functions of type void 
//and pass the opcode to the function
std::function<void(psp_gpr_t& psp_reg, opcode &op)> arm_preop[] = {
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {    //PRE_OP_LSL_IMM
        //printf("PRE_OP_LSL_IMM %d \n", op.imm);
        //loadReg(psp_reg, op.rs2);
        psp_reg = reg_alloc.getReg(op.rs2, psp_a1);
        if(op.imm) emit_sll(psp_reg, psp_reg, op.imm);
    },
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {    //PRE_OP_LSL_REG
        //printf("PRE_OP_LSL_IMM %d \n", op.imm);
        loadReg(psp_t0, op.rs2>>8);
        emit_sltiu(psp_t1,psp_t0, 32);        
        psp_reg = reg_alloc.getReg(op.rs2&0xff, psp_a1);
        emit_sllv(psp_reg, psp_reg, psp_t0);
        emit_movz(psp_reg, psp_zero, psp_t1);

    },
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {   //PRE_OP_LSR_IMM
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        if(op.imm) {
            psp_reg = reg_alloc.getReg(op.rs2, psp_a1);
            emit_srl(psp_reg, psp_reg, op.imm);
        }
        else {
            psp_reg = psp_zero;
            //emit_move(psp_reg, psp_zero);
        }
    },
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {   //PRE_OP_LSR_REG
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        loadReg(psp_t0, op.rs2>>8);
        emit_sltiu(psp_t1,psp_t0, 32);        
        psp_reg = reg_alloc.getReg(op.rs2&0xff, psp_a1);
        emit_srlv(psp_reg, psp_reg, psp_t0);
        emit_movz(psp_reg, psp_zero, psp_t1);
    },
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {   //PRE_OP_ASR_IMM
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        psp_reg = reg_alloc.getReg(op.rs2, psp_a1);
        emit_sra(psp_reg,psp_reg,op.imm ? op.imm : 31); 
    },
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {   //PRE_OP_ASR_REG
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        //TODO
    },
    [] (psp_gpr_t& psp_reg, opcode& op) -> void {   //PRE_OP_ROR_IMM
        //TODO
    },

    [] (psp_gpr_t& psp_reg, opcode& op) -> void {   //PRE_OP_NONE
        //TODO
    }
};

#define cpu (&ARMPROC)

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
    psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0); \
    psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false); \
    if (imm){ \
        if (!rev && is_##sign##16(op.imm)) \
            emit_##n_op_imm(dst, rs1, op.imm); \
        else{ \
            emit_li(psp_a1, op.imm); \
            if (!rev) \
                emit_##n_op(dst, rs1, psp_a1); \
            else \
                emit_##n_op(dst, psp_a1, rs1); \
        } \
    }else{ \
        psp_gpr_t rs2 = psp_zero; \
        arm_preop[op.preOpType](rs2 , op); \
        if (!rev) \
                emit_##n_op(dst, rs1, rs2); \
        else \
            emit_##n_op(dst, rs2, rs1); \
    } \
    if (op.rd == 15) \
        emit_sw(dst, RCPU, _next_instr); \
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

    return;


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
    uint32_t conditional_label = 0;
    uint32_t jump_sz = 0;

    switch(op._op){
  
        case OP_ITP:
        {

            conditional(
                store_flags();
                
                emit_li(psp_a0, op.rs1); 

                uint32_t optmizeDelaySlot = emit_SlideDelay();

                emit_jal(arm_instructions_set[INSTRUCTION_INDEX(op.rs1)]); 
                emit_Write32(optmizeDelaySlot);

                load_flags()
            )

            intr_instr++;
        }
        break; 
        case OP_AND:{ 
            const uint32_t weak_tag = (op.condition == 14) ? 0x10 : 0;
            int32_t regs[2] = { op.rd | 0x10, op.rs1 | weak_tag };
            regman.get(2, regs);
           //loadReg(psp_a0, op.rs1);

           conditional(
                emit_li(psp_v1, op.imm);
                emit_and(regs[0], regs[1], psp_v1)
            )
  
           regman.mark_dirty((psp_gpr_t)regs[0]);

           //regman.flush((psp_gpr_t)regs[0]);
        } 
        break;
 
        case OP_BIC:
            if (op.preOpType == PRE_OP_IMM)
                arm_bic<true, false>(op);
            else 
                arm_bic<false, false>(op);
        break;

        case OP_ORR:{
            const uint32_t weak_tag = (op.condition == 14) ? 0x10 : 0;
            int32_t regs[2] = { op.rd | 0x10, op.rs1 | weak_tag };
            regman.get(2, regs);
           //loadReg(psp_a0, op.rs1);

           conditional(
                emit_li(psp_v1, op.imm);
                emit_or(regs[0], regs[1], psp_v1)
            )
  
           regman.mark_dirty((psp_gpr_t)regs[0]);

           //regman.flush((psp_gpr_t)regs[0]);
        break;
        }

        case OP_EOR:
            if (op.preOpType == PRE_OP_IMM)
                arm_xor<true, false>(op);
            else 
                arm_xor<false, false>(op);

        break;

        case OP_ADD:
        {
            int32_t regs[2] = { op.rd | 0x10, op.rs1 };
            regman.get(2, regs);
            //loadReg(psp_a0, op.rs1);

            //printf("at 0x%x rd :%d\n", emit_getCurrAdr(), op.rd);

            conditional(
                emit_li(psp_v1, op.imm);
                emit_addu(regs[0], regs[1], psp_v1)
            )

            regman.mark_dirty((psp_gpr_t)regs[0]);

            //TODO IMPORTANT: FIND THE CONDITION WHERE WE CAN SKIP THE FORCED STORE 
            // Let me guess
            //regman.flush((psp_gpr_t)regs[0]);
            break;
        }
       case OP_SUB:
       {
            int32_t regs[2] = { op.rd | 0x10, op.rs1 };
            regman.get(2, regs);
           //loadReg(psp_a0, op.rs1);

           conditional(
                emit_li(psp_v1, op.imm);
                emit_subu(regs[0], regs[1], psp_v1)
            )
  
           regman.mark_dirty((psp_gpr_t)regs[0]);

           //regman.flush((psp_gpr_t)regs[0]);
        break;
    }

        
        case OP_RSB:
            if (op.preOpType == PRE_OP_IMM)
                arm_sub<true, true>(op);
            else 
                arm_sub<false, true>(op);
        break;
        case OP_MUL:
        {
            int32_t regs[3] = { op.rd | 0x10, op.rs1, op.rs2 };
            regman.get(3, regs);
            
            conditional(
                emit_mult(regs[1], regs[2]);
                emit_mflo(regs[0])
            )
    
            regman.mark_dirty((psp_gpr_t)regs[0]);

            //regman.flush((psp_gpr_t)regs[0]);
            break; 
        }

        case OP_MLA:
        {
            int32_t regs[4] = { op.rd | 0x10, op.rs1, op.rs2, op.imm };
            regman.get(4, regs);
            
            conditional(
                emit_mult(regs[1], regs[2]);
                emit_mflo(psp_t0);
                emit_addu(regs[0], psp_t0, regs[3])
            )
  
            regman.mark_dirty((psp_gpr_t)regs[0]);

            //regman.flush((psp_gpr_t)regs[0]);
            break; 
        }

        case OP_CLZ:
        {
            int32_t regs[2] = { op.rd | 0x10, op.rs1};
            regman.get(2, regs);

            conditional(emit_clz(regs[0], regs[1]));

            regman.mark_dirty((psp_gpr_t)regs[0]);

           // regman.flush((psp_gpr_t)regs[0]);
            break;
        }

        case OP_NEG:
        {
            psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
            psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a0);
            emit_negu(dst, rs1);
            break;
        }

        case OP_SWI:
        {
           reg_alloc.dealloc_all(); 
           emit_jal(cpu->swi_tab[op.rs1]); 
           emit_nop();
        }

        case OP_MOV:
        case OP_MVN:
        {

            //psp_gpr_t dst = reg_alloc.getReg(op.rd, psp_v0, false);
            int32_t regs[2] = { op.rd, -1};
            regman.get(2, regs);

            conditional(emit_li(regs[0], op.imm));
    
            regman.mark_dirty((psp_gpr_t)regs[0]);
            //regman.flush((psp_gpr_t)regs[0]);
            

            /*if (op.imm == 0 && op.preOpType == PRE_OP_IMM){
                emit_move(dst, psp_zero);
                break;
            }*/

            /*if (op.preOpType == PRE_OP_IMM){
                int32_t regs[1] = { op.rd | 0x10};
                regman.get(1, regs);

                emit_li(regs[0], op.imm);
                printf("op.imm: %d\n", op.imm);
        
                regman.mark_dirty((psp_gpr_t)regs[0]);
                regman.flush((psp_gpr_t)regs[0]);
                //emit_li(dst, op.imm);
            }else{*/
                /*psp_gpr_t rs1 = reg_alloc.getReg(op.rs1, psp_a1);

                if (op.preOpType != PRE_OP_NONE){
                    arm_preop[op.preOpType](rs1, op);

                    if (op._op == OP_MVN)
                        emit_not(dst, rs1);
                    else
                        emit_move(dst, rs1);
                }

                if (op.rd == 15)
                    emit_sw(dst, RCPU, _next_instr);*/
            //}
            
            break;
        }

        case OP_BXC:
        {
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

        case OP_NOP:
        case OP_FAST_MUL:
        case OP_STMIA:
        case OP_LSR_0:
        case OP_LDRH:
        case OP_STRH:
        case OP_LDR:
        case OP_STR:
        case OP_CMP:
        case OP_TST:
        case OP_AND_S:
        case OP_EOR_S:
        case OP_ORR_S:
        case OP_ADD_S:
        case OP_SUB_S:
        case OP_MOV_S:
        case OP_MVN_S:

        break;

    }
}

#include "thumb_jit.h"

//Already compiled and optimized block
#include "precompiled_ops.h"

template<int PROCNUM>
bool block::emitThumbBlock(){

    use_flags = true;

    reg_alloc.reset(); 

    emit_li(psp_fp, opcodes.front().op_pc);

    opcode last_op = opcodes.back();
    const bool islastITP = last_op._op == OP_ITP;

    for(opcode op : opcodes){

        const u8 isize = 2;

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

    opcode last_op = opcodes.back();

    intr_instr = 0;

    use_flags = uses_flags;

    char * found = strstr(compiled_functions_hash, block_hash);

    /*if (found != NULL) {
        return arm_compiledOP[(found - compiled_functions_hash) / 51](PROCNUM);
    }*/

    start_block = emit_getCurrAdr();
    emit_li(psp_fp, opcodes.front().op_pc);
    
    load_flags();

    regman.reset();

    int dyna_count = 0;
    for(opcode& op : opcodes) {
        if (op._op == OP_ADD || op._op == OP_SUB || op._op == OP_MOV || op._op == OP_MVN || op._op == OP_AND || op._op == OP_ORR || op._op == OP_EOR ||  op._op == OP_MUL || op._op == OP_MLA || op._op == OP_CLZ )
            dyna_count++;
    }

    if (dyna_count > 3) {
        printf("dyna_count: %d\n", dyna_count);
        printf("0x%x\n", (u32)emit_getCurrAdr());
    }


    for(opcode op : opcodes){ 
        const u8 isize =  4;

        if (op._op == OP_ITP || op._op == OP_SWI){
            regman.flush_all();
            regman.reset();
        }

        if (last_op.op_pc != op.op_pc)
            emit_prefetch(isize, op.rs1 == 15 || op.rs2 == 15, op._op == OP_ITP);
        else {
            //Last op has always to save 
            emit_prefetch(isize, true, true);
        }

        emitARMOP<PROCNUM>(op);                
    } 

    regman.flush_all();
    regman.reset();

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


