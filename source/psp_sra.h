struct reg_allocation
{
    private:

        int reg_idx = 0;
        
        int nds_psp[15] {-1};
        int psp_nds[15] {-1};


    public:

        void alloc_reg(uint32_t reg, bool load = true) {
            const psp_gpr_t psp_regs[] = {psp_s0, psp_s1, psp_s2, psp_s3, psp_s4};
            
            if (reg_idx < 5) {

                //printf("Allocating reg %d to %d\n", reg, psp_regs[reg_idx]);
                //emit_mov(psp_regs[reg_idx], reg); load
                if (load) loadReg(psp_regs[reg_idx], reg);

                nds_psp[reg] = psp_regs[reg_idx];
                psp_nds[psp_regs[reg_idx]] = reg;
                reg_idx++;
            }
        }

        void alloc_regs(opcode op) {
            if (op._op == OP_ITP || op._op == OP_SWI) return;

            if (op.rs1 != -1 && nds_psp[op.rs1] == -1) alloc_reg(op.rs1);
            if (op.rs2 != -1 && nds_psp[op.rs2] == -1) alloc_reg(op.rs2);
            if (op.rd != -1 && nds_psp[op.rd] == -1) alloc_reg(op.rd, (op.condition == -2));
        }

        void free_reg(uint32_t reg) {
            psp_nds[nds_psp[reg]] = -1;
            nds_psp[reg] = -1;
            reg_idx--;
        }

        void dealloc_all() {
            for (int i = 0; i < 16; i++) {
                if (nds_psp[i] != -1) {
                    //printf("dealloc %d, %d  at 0x%x\n", (psp_gpr_t)nds_psp[i], psp_nds[nds_psp[i]], emit_getCurrAdr());
                    storeReg((psp_gpr_t)nds_psp[i], psp_nds[nds_psp[i]]);
                    free_reg(i);
                }
            }
        }

        void dealloc(u32 reg) {
            if (nds_psp[reg] != -1) {
                //printf("dealloc %d, %d  at 0x%x\n", (psp_gpr_t)nds_psp[i], psp_nds[nds_psp[i]], emit_getCurrAdr());
                storeReg((psp_gpr_t)nds_psp[reg], psp_nds[nds_psp[reg]]);
                free_reg(reg);
            }
        }

        void free_regs(opcode op) {
            if (op.rd != -1 && psp_nds[op.rd] == -1) free_reg(op.rd);
            if (op.rs1 != -1 && psp_nds[op.rs1] == -1) free_reg(op.rs1);
            if (op.rs2 != -1 && psp_nds[op.rs2] == -1) free_reg(op.rs2);
        }


        psp_gpr_t getReg(uint32_t reg, psp_gpr_t _default, bool alloc = true) {
            if (reg == -1) {
                printf("Invalid reg passed to get_reg %d, %d\n", reg, _default);
                return _default;
            }

            if (nds_psp[reg] == -1) {
                //printf("Using default reg: used %d\n", reg_idx);
                if (alloc) loadReg(_default, reg);
                return _default;
            }

            //printf("Reg cache hit\n");

            return (psp_gpr_t)nds_psp[reg];
        }

        void reset() {
            reg_idx = 0;
            for (int i = 0; i < 15; i++) {
                nds_psp[i] = -1;
                psp_nds[i] = -1;
            }
        }
};