#include "blockdecoder.h"

#include "PSP/emit/psp_emit.h"
#include "mips_code_emiter.h"
#include "armcpu.h"
#include <functional>

block currentBlock;  

bool instr_does_prefetch(u32 opcode);

const char * compiled_functions_hash = 
    ">:1:05:8FDA19B2:C2AF4F28:998B99C6:F0596DB3:2FD2AA9B" //used by yoshy island ingame
    ">:1:07:88DD1AED:096B3BDA:7857790C:6D443C78:A4ADDB58" //a lot used in pokemon diamond 
    ;

//make an array of pointers to functions of type void 
//and pass the opcode to the function
std::function<void(opcode &op)> arm_preop[] = {
    [] (opcode& op) -> void { }, //PRE_OP_NONE
    [] (opcode& op) -> void {    //PRE_OP_LSL_IMM
        //printf("PRE_OP_LSL_IMM %d \n", op.imm);
        loadReg(psp_a1, op.rs2);
        if(op.imm) emit_sll(psp_a1, psp_a1, op.imm);
    },
    [] (opcode& op) -> void {    //PRE_OP_LSL_REG
        //printf("PRE_OP_LSL_IMM %d \n", op.imm);
        loadReg(psp_t0, op.rs2>>8);
        emit_sltiu(psp_t1,psp_t0, 32);        
        loadReg(psp_a1, op.rs2&0xff);
        emit_sllv(psp_a1, psp_a1, psp_t0);
        emit_movz(psp_a1, psp_zero, psp_t1);

    },
    [] (opcode& op) -> void {   //PRE_OP_LSR_IMM
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        if(op.imm) {
            loadReg(psp_a1, op.rs2);
            emit_srl(psp_a1, psp_a1, op.imm);
        }
        else {
            emit_move(psp_a1, psp_zero);
        }
    },
    [] (opcode& op) -> void {   //PRE_OP_LSR_REG
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        loadReg(psp_t0, op.rs2>>8);
        emit_sltiu(psp_t1,psp_t0, 32);        
        loadReg(psp_a1, op.rs2&0xff);
        emit_srlv(psp_a1, psp_a1, psp_t0);
        emit_movz(psp_a1, psp_zero, psp_t1);
    },
    [] (opcode& op) -> void {   //PRE_OP_ASR_IMM
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        loadReg(psp_a1, op.rs2);
        emit_sra(psp_a1,psp_a1,op.imm ? op.imm : 31); 
    },
    [] (opcode& op) -> void {   //PRE_OP_ASR_REG
        //printf("PRE_OP_LSR_IMM %d \n", op.imm);
        //TODO
    },
    [] (opcode& op) -> void {   //PRE_OP_ROR_IMM
        //TODO
    }
};

uint32 emit_Halfbranch(int cond)
{
	static const uint8 cond_bit[] = {0x40, 0x40, 0x20, 0x20, 0x80, 0x80, 0x10, 0x10};

	if(cond < 8)
	{
      emit_andi(psp_t0, psp_k1, cond_bit[cond]);

      emit_nop();
      emit_nop();
      return emit_getPointAdr() - 8;
	}

   switch (cond){

      case 8:  
      case 9:
         emit_ext(psp_a1,psp_k1,6,5);
         emit_xori(psp_t0,psp_a1,0b01);

         emit_nop();
         emit_nop();
      break;

      case 10:
      case 11:

         emit_ext(psp_a1,psp_k1,7,7);
         emit_ext(psp_at,psp_k1,4,4);

         emit_xor(psp_t0,psp_a1,psp_at);

         emit_nop();
         emit_nop();

      break;

      case 12:
      case 13:

         emit_ext(psp_a1,psp_k1,7,6);
         emit_ext(psp_at,psp_k1,4,3);

         emit_andi(psp_at,psp_at,0b10);
         emit_xor(psp_t0,psp_a1,psp_at);

         emit_nop();
         emit_nop();
      break;

      default:
      //printf("emit_Halfbranch: invalid cond %d\n", cond);
      return 0;
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

#define gen_nativeOP(opType, n_op, n_op_imm) \
template <bool imm, int rev> void arm_##opType(opcode &op){ \
    loadReg(psp_a0, op.rs1); \
    if (imm){ \
        if (!rev && fitsInShort(op.imm)) \
            emit_##n_op_imm(psp_v0, psp_a0, op.imm); \
        else{ \
            emit_li(psp_a1, op.imm); \
            if (!rev) \
                emit_##n_op(psp_v0, psp_a0, psp_a1); \
            else \
                emit_##n_op(psp_v0, psp_a1, psp_a0); \
        } \
    }else{ \
        arm_preop[op.preOpType](op); \
        if (!rev) \
                emit_##n_op(psp_v0, psp_a0, psp_a1); \
        else \
            emit_##n_op(psp_v0, psp_a1, psp_a0); \
    } \
    if (op.rd != -1) { \
        storeReg(psp_v0, op.rd); \
        if (op.rd == 15) \
            emit_sw(psp_v0, RCPU, _next_instr); \
    } \
}

gen_nativeOP(and, and, andi);
gen_nativeOP(or, or, ori);
gen_nativeOP(xor, xor, xori);
gen_nativeOP(add, addu, addiu);
gen_nativeOP(sub, subu, subiu);

void block::optimize_basicblock(){
    opcode *prev_op = 0;
    opcode *prev_ITP = 0;

    for(opcode& op : opcodes){

        if (prev_ITP && op.condition == -1  && op._op == OP_ITP){
            op.extra_flags = EXTFL_SKIPSAVEFLAG;
        }
        
        if (prev_ITP && prev_ITP->condition == -1) prev_ITP->extra_flags = EXTFL_SKIPLOADFLAG;

        if (op._op == OP_ITP){
            prev_op = 0;
            prev_ITP = &op;
            continue;
        }

        prev_ITP = 0;

        continue;

        //check for useless operation
        if (op.preOpType == PRE_OP_IMM && (op._op == OP_ORR || op._op == OP_EOR || op._op == OP_SUB) && (op.imm == 0)) {
            //printf("Translated to move - OP: %x\n", op._op);
            op._op = OP_MOV;
            op.preOpType = PRE_OP_NONE;
            continue;
        }

        if (op._op == OP_AND && op.preOpType == PRE_OP_IMM && (op.imm == 0)) {
            //printf("Translated AND to mov0\n");
            op._op = OP_MOV;
            op.imm = 0;
            op.preOpType = PRE_OP_IMM;
            continue;
        }

        if (op._op == OP_RSB && (op.imm == 0)) {
            //printf("Translated RSB to neg\n");
            op._op = OP_NEG;
            continue;
        }

        if (op.preOpType == PRE_OP_LSR_IMM && (op.imm == 0)) {
            //printf("Translated PRE_OP_LSR_IMM OP to MOV\n");
            op._op = OP_MOV;
            op.preOpType = PRE_OP_NONE;
            continue;
        }

        if (op._op == OP_MOV && op.rs1 == op.rd && op.preOpType == PRE_OP_NONE) {
            op._op = OP_NOP;
            op.rd = 0;
            continue;
        }

        continue;

        if (prev_op && prev_op->_op == OP_MOV && prev_op->rd == op.rs2 && prev_op->preOpType == PRE_OP_IMM){
            //printf("Could be replaced with pre_op imm\n");

            //printf("prev_op imm: %x op: %d\n", prev_op->imm, op._op);
           
           //there are a lot of useless mult moves we can delete them with this 
           /*if (prev_op->rd == op.rd && op._op == OP_MUL) {
                //printf("Removed useless op, same rd\n");
                prev_op->_op = OP_NOP;
                op.imm = prev_op->imm;
                op.rs2 = -1;
                continue;
            }*/

            if(op._op >= OP_ADD && op._op <= OP_SUB || op._op == OP_RSB){

                if (op.preOpType == PRE_OP_LSL_IMM){
                    //printf("Translated LSL to IMM op: %d\n", op._op);
                    op.imm = prev_op->imm << op.imm;
                    op.preOpType = PRE_OP_IMM;
                    op.rs2 = -1;
                    continue;
                }

                if (op.preOpType == PRE_OP_LSR_IMM){
                    //printf("Translated LSR to IMM op: %d\n", op._op);
                    op.imm = prev_op->imm >> op.imm;
                    op.preOpType = PRE_OP_IMM;
                    op.rs2 = -1;
                    continue;
                }


                if (op.rd == prev_op->rd && op.preOpType == PRE_OP_LSR_REG) {
                    prev_op->_op = OP_NOP;
                    op.imm = prev_op->imm << (prev_op->imm < 32 ? prev_op->imm : 31);
                    op.rs1 &= 0xF;
                    op.preOpType = PRE_OP_IMM;
                    continue;
                }

                if (op.rd == prev_op->rd && op.preOpType == PRE_OP_LSL_REG) {
                    prev_op->_op = OP_NOP;
                    op.imm = prev_op->imm >> (prev_op->imm < 32 ? prev_op->imm : 31);
                    op.rs1 &= 0xF;
                    op.preOpType = PRE_OP_IMM;
                    continue;
                }

                if (op.preOpType == PRE_OP_LSR_REG){
                    op.imm = prev_op->imm;
                    op.preOpType = PRE_OP_LSR_IMM;
                    continue;
                }

                    if (op.preOpType == PRE_OP_LSL_REG){
                    op.imm = prev_op->imm;
                    op.preOpType = PRE_OP_LSL_IMM;
                    continue;
                }

            }

            #if 0
                if (op._op == OP_ORR && prev_op->imm == 0) {
                    printf("Translated OR OP to MOV TYPE:%d\n", op.preOpType);
                    op._op = OP_MOV;
                    op.preOpType = PRE_OP_NONE;
                }

                if (op._op == OP_AND && prev_op->imm == 0) {
                    printf("Translated AND OP to MOV 0\n");
                    op._op = OP_MOV;
                    op.imm = 0;
                    op.preOpType = PRE_OP_IMM;
                }

                if (op._op == OP_ADD && prev_op->imm == 0) {
                    printf("Translated ADD OP to MOV\n");
                    op._op = OP_MOV;
                    op.preOpType = PRE_OP_NONE;
                }

                if (op._op == OP_SUB && prev_op->imm == 0) {
                    printf("Translated SUB OP to MOV\n");
                    op._op = OP_MOV;
                    op.preOpType = PRE_OP_NONE;
                }
            #endif
        }

        prev_op = &op;
    }
}

extern "C" void set_sub_flags();
extern "C" void set_and_flags();
extern "C" void set_op_logic_flags();

#define cpu (&ARMPROC)

template<int PROCNUM>
static u16 FASTCALL _LDRH(u32 adr)
{
	return READ16(cpu->mem_if->data, adr);
}

template<int PROCNUM>
static u16 FASTCALL _LDRH_REG(u32 regs)
{
    u32 adr = (u32)cpu->R[(regs>>4)&0xf] + (u32)cpu->R[regs&0xf];
	return READ16(cpu->mem_if->data, adr);
}

template<int PROCNUM>
static void FASTCALL _STRH(u32 regs, u32 imm)
{
    u32 adr = (u32)cpu->R[regs>>8] + imm;
    u32 data = (u32)cpu->R[regs&0xFF];
	WRITE16(cpu->mem_if->data, adr, data);
}

template<int PROCNUM>
static void FASTCALL _STRH_REG(u32 regs)
{
    u32 adr = (u32)cpu->R[(regs>>8)&0xf] + (u32)cpu->R[(regs>>4)&0xf];
    u32 data = (u32)cpu->R[regs&0xF];
	WRITE16(cpu->mem_if->data, adr, data);
}

/*template<int PROCNUM>
void AssingReadFunction(u32 addr){
	unsigned 	o_ra;
	extern u8 *CodeCache;

	//Execute the patch at the end (overwrite ra addr inside the sp)
	asm volatile("addiu $2, $31, -8"); 
	asm volatile("sw $2, 0x14($29)");

	//Get the current ra 
	asm volatile("sw $31, %0":"=m"(o_ra));

	u32 _ptr = emit_Set((o_ra - 8) - (u32)&CodeCache);

	if (){
        emit_lh(psp_v0, psp_a0, mem_if_data);
    }else{
        emit_jal(_OP_LDRH<PROCNUM>);
    }

	emit_Set(_ptr);

	//make_address_range_executable(o_ra - 8, o_ra - 4);
	u32 addr_start = o_ra - 8;
	__builtin_allegrex_cache(0x1a, addr_start);
	__builtin_allegrex_cache(0x08, addr_start);
}*/

int intr_instr = 0;

bool flag_loaded = false;
bool use_flags = false;
bool islast_op = false;

void load_flags(){
    if (use_flags && !flag_loaded){
        emit_lbu(psp_k1, RCPU, _flags+3);
        flag_loaded = true;
    }
}

void store_flags(){
    if (use_flags && flag_loaded){
        emit_sb(psp_k1, RCPU, _flags+3);
        flag_loaded = false;
    }
}

template<int PROCNUM>
void emitARMOP(opcode& op){
    switch(op._op){
        case OP_ITP:
        {
            if (!(op.extra_flags & EXTFL_SKIPSAVEFLAG)) 
                store_flags();

            emit_li(psp_a0, op.rs1); 

            uint32_t optmizeDelaySlot = emit_SlideDelay();

            emit_jal(arm_instructions_set[PROCNUM][INSTRUCTION_INDEX(op.rs1)]);
            emit_Write32(optmizeDelaySlot);

            if (!(op.extra_flags & EXTFL_SKIPLOADFLAG) && !islast_op)
                load_flags();

            intr_instr++;
        }
        break;
        case OP_AND:
            if (op.preOpType == PRE_OP_IMM)
                arm_and<true, false>(op);
            else 
                arm_and<false, false>(op);
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
            printf("ADDR: 0x%x\n", emit_getCurrAdr());

            emit_li(psp_a2, 0x3ffffc);                  // load mem mask

            loadReg(psp_a0, op.rs2);                    // load dst stack addr

            for(int j = 0; j<16; j++)
                if(BIT_N(op.rs1, j))
                {
                    loadReg(psp_a1, j);                 // load reg val

                    emit_and(psp_a0, psp_a0, psp_a2);   // a0 = mem addr & mask

                    emit_addu(psp_t0, psp_k0, psp_a0);  // t0 = mem ptr + mem addr
                    emit_sw(psp_a1, psp_t0, mem_if_data);         // store a1 to 0(t0)

                    emit_addiu(psp_a0,psp_a0,4);        // add 4 to mem addr
                }
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

        case OP_LDRH:
            //currentBlock.noReadWriteOP = false;
            if (op.imm == 0){
                emit_jal(_LDRH<PROCNUM>);
                loadReg(psp_a0, op.rs1);
                storeReg(psp_v0, op.rd);
            }else if (op.preOpType == PRE_OP_PRE_REG){
                emit_movi(psp_a0, op.rs1<<4 | op.rs2);
                emit_jal(_LDRH_REG<PROCNUM>);
                storeReg(psp_v0, op.rd);
            }else{
                loadReg(psp_a0, op.rs1);
                emit_jal(_LDRH<PROCNUM>);
                emit_addiu(psp_a0, psp_a0, op.imm);
                storeReg(psp_v0, op.rd);
            }
        break;

        case OP_STRH:
            //To help the cache we pack the arm registers in a single psp register and unpack them in the function  
            currentBlock.noReadWriteOP = false;
            if (op.preOpType == PRE_OP_PRE_REG){
                emit_jal(_STRH_REG<PROCNUM>);
                emit_movi(psp_a0, op.rs1<<8 | op.rd << 4 | op.rs2);
            }else{
                emit_movi(psp_a0, op.rs1<<8 | op.rs2);
                emit_jal(_STRH<PROCNUM>);
                emit_movi(psp_a1, op.imm);
            }
        break;

        case OP_SUB_S:
        case OP_CMP:
        {

            if (op.preOpType == PRE_OP_IMM){
                arm_sub<true, false>(op);
                emit_li(psp_a1, op.imm);
            }else
                arm_sub<false, false>(op);

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
            uint32_t optmizeDelaySlot = emit_SlideDelay();
            emit_jal(cpu->swi_tab[op.rs1]);
            emit_Write32(optmizeDelaySlot);
        }

        case OP_MOV:
        case OP_MVN:
        case OP_MVN_S:
        case OP_MOV_S:


            if (op.imm == 0 && op.preOpType == PRE_OP_IMM){
                storeReg(psp_zero, op.rd);
                
            //is that valid also for mvn_s? not sure
                if (op._op == OP_MOV_S || op._op == OP_MVN_S)
                    emit_ori(psp_k1, psp_k1, 0x40);     
                
                break;
            }

            if (op.preOpType == PRE_OP_IMM){
                emit_li(psp_a1, op.imm);
            }else{
                if (op.preOpType == PRE_OP_NONE){
                    loadReg(psp_a1, op.rs1);
                }else
                    arm_preop[op.preOpType](op);

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
    }
}

template<int PROCNUM> 
void emitThumbOP(opcode& op){
    switch(op._op) {
        case OP_ITP:
        {
            store_flags();

            emit_jal(thumb_instructions_set[PROCNUM][op.rs1 >> 6]);

            emit_movi(psp_a0,op.rs1&0xFFFF); 

            intr_instr++;
        }
        break;

        case OP_AND:
        {
            loadReg(psp_a0, op.rd);
            loadReg(psp_a1, op.rs1);
            emit_and(psp_v0, psp_a0, psp_a1);
            
            emit_jal(set_op_logic_flags);
            storeReg(psp_v0, op.rd);            
        }
        break;

        case OP_SUB:
        {
            if (op.preOpType == PRE_OP_IMM){
                if (op.imm){
                    loadReg(psp_a0, op.rs1);
                    emit_subiu(psp_v0, psp_a0, op.imm);
                }else
                    loadReg(psp_v0, op.rs1);
            }else{
                loadReg(psp_a0, op.rs1);
                loadReg(psp_a1, op.rs2);
                emit_subu(psp_v0, psp_a0, psp_a1);
            }

            emit_jal(set_sub_flags);
            storeReg(psp_v0, op.rd);  
        }
        break;

        case OP_ADD:
        {
            if (op.preOpType == PRE_OP_IMM){
                if (op.imm){
                    loadReg(psp_a0, op.rs1);
                    emit_addiu(psp_v0, psp_a0, op.imm);
                }else
                    loadReg(psp_v0, op.rs1);
            }else{
                loadReg(psp_a0, op.rs1);
                loadReg(psp_a1, op.rs2);
                emit_addu(psp_v0, psp_a0, psp_a1);
            }
        
            emit_jal(set_and_flags);
            storeReg(psp_v0, op.rd);            
        }
        break;

        case OP_EOR:
        {
            loadReg(psp_a0, op.rd);
            loadReg(psp_a1, op.rs1);
            emit_xor(psp_v0, psp_a0, psp_a1);
            
            emit_jal(set_op_logic_flags);
            storeReg(psp_v0, op.rd);            
        }
        break;

        case OP_ORR:
        {
            loadReg(psp_a0, op.rd);
            loadReg(psp_a1, op.rs1);
            emit_or(psp_v0, psp_a0, psp_a1);
            
            emit_jal(set_op_logic_flags);
            storeReg(psp_v0, op.rd);            
        }
        break;

        case OP_TST:
        {
            loadReg(psp_a0, op.rs1);
            loadReg(psp_a1, op.rs2);
            
            emit_jal(set_and_flags);
            emit_and(psp_v0, psp_a0, psp_a1);            
        }
        break;

        case OP_SWI:
        {
            uint32_t optmizeDelaySlot = emit_SlideDelay();
            emit_jal(cpu->swi_tab[op.rs1]);
            emit_Write32(optmizeDelaySlot);
        }

        case OP_CMP:
        {
            if (op.preOpType == PRE_OP_IMM){
                if (op.imm){
                    loadReg(psp_a0, op.rs1);
                    emit_jal(set_sub_flags);
                    emit_subiu(psp_v0, psp_a0, op.imm);
                }else{
                    emit_jal(set_sub_flags);
                    loadReg(psp_v0, op.rs1);
                }
            }else{
                loadReg(psp_a0, op.rs1);
                loadReg(psp_a1, op.rs2);
                
                emit_jal(set_sub_flags);
                emit_subu(psp_v0, psp_a0, psp_a1);  
            }          
        }
        break;

        case OP_MUL:
        {
            loadReg(psp_a0, op.rd);
            loadReg(psp_a1, op.rs1);
            emit_mult(psp_a0, psp_a1);

            emit_mflo(psp_v0);
            
            emit_jal(set_op_logic_flags);
            storeReg(psp_v0, op.rd);             
        }
        break;

        case OP_MOV:
        {
            if (op.preOpType == PRE_OP_IMM){

                if (op.imm == 0){
                    storeReg(psp_zero, op.rd);
                    emit_ori(psp_k1, psp_k1, 0x40); 
                    break;
                }
                
                emit_li(psp_v0, op.imm);
                emit_jal(set_op_logic_flags);
                storeReg(psp_v0, op.rd);
            }else{
                loadReg(psp_v0, op.rs1);
                if (op.preOpType != NOFLAGS) 
                    emit_jal(set_op_logic_flags);
                else
                    emit_sw(psp_v0, psp_k0, _next_instr);
                storeReg(psp_v0, op.rd);
            }
        }
        break;

        case OP_STRH:
        {
            emit_movi(psp_a0, op.rs1<<8 | op.rs2);
            emit_jal(_STRH<PROCNUM>);
            emit_movi(psp_a1, op.imm);
        }
        break;

        case OP_LDRH:
        {
            loadReg(psp_a0, op.rs1);
            emit_jal(_LDRH<PROCNUM>);
            emit_addiu(psp_a0, psp_a0, op.imm);
            storeReg(psp_v0, op.rd);
        }
        break;

        case OP_MVN:
        {
            loadReg(psp_a0, op.rs1);
            emit_not(psp_v0, psp_a0);
            
            emit_jal(set_op_logic_flags);
            storeReg(psp_v0, op.rd);            
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

        if (PROCNUM)
            currentBlock.emitArmBranch<1>();
        else
            currentBlock.emitArmBranch<0>();

        return true;
    }, 
    [] (bool PROCNUM) -> bool { 

        if (PROCNUM)
            emit_jal(_LDRH<1>);
        else
            emit_jal(_LDRH<0>);

        emit_lw(psp_a0, psp_k0, 0x38);

        emit_andi(psp_v0, psp_v0, 0x4000);
        emit_sw(psp_v0, psp_k0, 0x10); 

        emit_jal(set_and_flags);
        emit_movi(psp_a1, 0x4000);

        emit_andi(psp_t0, psp_k1, 0x40);

        emit_bnel(psp_t0, psp_zero, emit_getCurrAdr() + 44);
        {
            emit_nop();

            if (PROCNUM)
                emit_jal(_LDRH<1>);
            else
                emit_jal(_LDRH<0>);

            emit_lw(psp_a0, psp_k0, 0x38);

            emit_ori(psp_v0, psp_v0, 0xc000);

            emit_sw(psp_v0, psp_k0, 0x10); 

            emit_movi(psp_a0, 0xa00);
            
            if (PROCNUM)
                emit_jal(_STRH<1>);
            else
                emit_jal(_STRH<0>);

            emit_move(psp_a1, psp_zero);

            emit_lw(psp_a0, psp_k0, 0x34);
            emit_sw(psp_a0, psp_k0, 0x14);
        }

        if (PROCNUM)
            currentBlock.emitArmBranch<1>();
        else
            currentBlock.emitArmBranch<0>();

        return false;
    }
};

template<int PROCNUM>
bool block::emitThumbBlock(){

    use_flags = true;

    if (opcodes.size() == 1){
        opcode op = opcodes.front();

        emit_li(psp_fp, op.op_pc + 2);
        emit_sw(psp_fp, psp_k0, _next_instr);

        emit_addiu(psp_at, psp_fp, 2);
        emit_sw(psp_at, psp_k0, _R15);

        if (op._op != OP_ITP) load_flags();

        emitThumbOP<PROCNUM>(op);

        if (op._op != OP_ITP) store_flags();

        return false;
    }else
        emit_li(psp_fp, opcodes.front().op_pc);

    opcode last_op = opcodes.back();
    const bool islastITP = last_op._op == OP_ITP;

    for(opcode op : opcodes){

        const u8 isize = /*(op._op == OP_NOP && op.rd == 0) ? 4 :*/ 2;

        if (op._op != OP_ITP) load_flags();

        if (last_op.op_pc != op.op_pc)
            emit_prefetch(isize, op.rs1 == 15 || op.rs2 == 15, op._op == OP_ITP);
        else
            //Last op has always to save 
            emit_prefetch(isize, true, true);
        
        emitThumbOP<PROCNUM>(op);

    }
    
    store_flags();


    //possible idle loop, do more checks here
    return false;

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

   if (opcodes.size() == 1){
        opcode op = opcodes.front();
        
        emit_li(psp_fp, op.op_pc + 4);
        emit_sw(psp_fp, psp_k0, _next_instr);

        emit_addiu(psp_at, psp_fp, 4);
        emit_sw(psp_at, psp_k0, _R15);
        
        if (op.condition != -1 || op._op >= OP_CMP) load_flags();

        conditional(emitARMOP<PROCNUM>(op))

        if (op.condition != -1 || op._op >= OP_CMP) store_flags();

        return (intr_instr == 0);
    }else
        emit_li(psp_fp, opcodes.front().op_pc);
    

    opcode last_op = opcodes.back();
    const bool islastITP = last_op._op == OP_ITP;

    for(opcode op : opcodes){

        //TODO: with nop and skip prefetch, the next_istr and r15 gets a wrong value
        const u8 isize = /*(op._op == OP_NOP && op.rd == 0) ? 8 :*/ 4;

        if (last_op.op_pc != op.op_pc)
            emit_prefetch(isize, op.rs1 == 15 || op.rs2 == 15, op._op == OP_ITP);
        else
            //Last op has always to save 
            emit_prefetch(isize, true, true);

        if (op.condition != -1 || op._op >= OP_CMP)
            load_flags();

        conditional(emitARMOP<PROCNUM>(op))
    }

    store_flags();

    //possible idle loop, do more checks here
    return (intr_instr == 0) && currentBlock.noReadWriteOP && (currentBlock.opcodes.size() < 6);
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
