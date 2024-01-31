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
        break;
    }
}

template bool block::emitThumbBlock<0>();
template bool block::emitThumbBlock<1>();