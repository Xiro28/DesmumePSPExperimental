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
