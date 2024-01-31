struct reg_allocation
{
    private:

        int reg_idx = 0;
        
        int nds_psp[16] {-1};
        int psp_nds[16] {-1};
        bool dirty [16] {false};

    public:

        void alloc_reg(uint32_t reg, bool load = true) {
            const psp_gpr_t psp_regs[] = {psp_s0, psp_s1, psp_s2, psp_s3, psp_s4};
            
            if (reg_idx < 5) {

                //printf("Allocating reg %d to %d\n", reg, psp_regs[reg_idx]);
                //emit_mov(psp_regs[reg_idx], reg); load
                if (load) loadReg(psp_regs[reg_idx], reg);

                nds_psp[reg] = psp_regs[reg_idx];
                psp_nds[psp_regs[reg_idx]] = reg;
                dirty[reg] = false;
                reg_idx++;
            }
        }

        void alloc_regs(opcode op) {
            if (op._op == OP_ITP || op._op == OP_SWI) return;

            if (op.rs1 != -1 && nds_psp[op.rs1] == -1) alloc_reg(op.rs1);
            if (op.rs2 != -1 && nds_psp[op.rs2] == -1) alloc_reg(op.rs2);
            if (op.rd != -1 && nds_psp[op.rd] == -1) alloc_reg(op.rd, (op.condition == -2));
        }

        void alloc_regs(opcode op, bool allocate_rd) {
            if (op._op == OP_ITP || op._op == OP_SWI) return;

            if (op.rs1 != -1 && nds_psp[op.rs1] == -1) alloc_reg(op.rs1);
            if (op.rs2 != -1 && nds_psp[op.rs2] == -1) alloc_reg(op.rs2);
            if (op.rd != -1 && nds_psp[op.rd] == -1 && allocate_rd) alloc_reg(op.rd);
        }

        void end_op(opcode& op){
            if (nds_psp[op.rd] == -1) storeReg(psp_v0, op.rd);
            else {
                dirty[op.rd] = true;
            }
        }

        void free_reg(uint32_t reg) {
            psp_nds[nds_psp[reg]] = -1;
            nds_psp[reg] = -1;
            dirty[reg] = false;
            reg_idx--;
        }

        void dealloc_all() {
            for (int i = 0; i < 16; i++) {
                if (nds_psp[i] != -1) {
                    printf("dealloc %d, %d  at 0x%x\n", (psp_gpr_t)nds_psp[i], psp_nds[nds_psp[i]], emit_getCurrAdr());
                    if (dirty[i]) storeReg((psp_gpr_t)nds_psp[i], psp_nds[nds_psp[i]] - 1);
                    free_reg(i);
                }
            }
        }

        void dealloc(u32 reg) {
            if (nds_psp[reg] != -1) {
                //printf("dealloc %d, %d  at 0x%x\n", (psp_gpr_t)nds_psp[i], psp_nds[nds_psp[i]], emit_getCurrAdr());
                if (dirty[reg])  storeReg((psp_gpr_t)nds_psp[reg], psp_nds[nds_psp[reg]]);
                free_reg(reg);
            }
        }

        void free_regs(opcode op) {
            if (op.rd != -1 && psp_nds[op.rd] == -1) free_reg(op.rd);
            if (op.rs1 != -1 && psp_nds[op.rs1] == -1) free_reg(op.rs1);
            if (op.rs2 != -1 && psp_nds[op.rs2] == -1) free_reg(op.rs2);
        }

        bool isAllocated(uint32_t reg) {
            return nds_psp[reg] != -1;
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
            for (int i = 0; i < 16; i++) {
                nds_psp[i] = -1;
                psp_nds[i] = -1;
                dirty[i] = false;
            }
        }
};

#include <iostream>
#include <vector>
#include <algorithm>
#include <unordered_map>

class register_manager
{
   public:
      void reset()
      {
         memset(mapping, 0xFF, sizeof(mapping));
         memset(usage_tag, 0, sizeof(usage_tag));
         memset(dirty, 0, sizeof(dirty));
         memset(weak, 0, sizeof(weak));
         next_usage_tag = 1;
      }

      bool is_usable(psp_gpr_t reg) const
      {
         static const uint32_t USE_MAP = 0x1F0000;
         return (USE_MAP & (1 << reg)) ? true : false;
      }

   private:
      psp_gpr_t find(uint32_t emu_reg_id)
      {
         for (int i = psp_s0; i != psp_s5; i ++)
         {
            if (mapping[i] == emu_reg_id)
            {
               usage_tag[i] = next_usage_tag ++;
               return (psp_gpr_t) i;
            }
         }

         return psp_null;
      }

      int32_t get_loaded(uint32_t emu_reg_id, bool no_read)
      {
         psp_gpr_t current = find(emu_reg_id);

         if (current >= 0)
         {
            if (weak[current] && !no_read)
            {
               read_emu(current, emu_reg_id);
               weak[current] = false;
            }
         }

         return current;
      }

      psp_gpr_t get_oldest()
      {
         psp_gpr_t result = psp_s0;
         uint32_t lowtag = 0xFFFFFFFF;

         for (int i = psp_s0; i != psp_s5; i ++)
         {
            if (usage_tag[i] < lowtag)
            {
               lowtag = usage_tag[i];
               result = (psp_gpr_t) i;
            }
         }

         return result;
      }

   public:
      void get(uint32_t reg_count, int32_t* emu_reg_ids)
      {
         assert(reg_count < 5);
         bool found[5] = { false, false, false, false, false };

         // Find existing registers
         for (uint32_t i = 0; i < reg_count; i ++)
         {
            if (emu_reg_ids[i] < 0)
            {
               found[i] = true;
            }
            else
            {
               int32_t current = get_loaded(emu_reg_ids[i] & 0xF, emu_reg_ids[i] & 0x10);
               if (current >= 0)
               {
                  emu_reg_ids[i] = current;
                  found[i] = true;
               }
            }
         }

         // Load new registers
         for (uint32_t i = 0; i < reg_count; i ++)
         {
            if (!found[i])
            {
               // Search register list again, in case the same register is used twice
               int32_t current = get_loaded(emu_reg_ids[i] & 0xF, emu_reg_ids[i] & 0x10);
               if (current >= 0)
               {
                  emu_reg_ids[i] = current;
                  found[i] = true;
               }
               else
               {
                  // Read the new register
                  psp_gpr_t result = get_oldest();
                  flush(result);

                  //printf("Loading register %d into %d at 0x%x\n", emu_reg_ids[i] & 0xF, result, emit_getCurrAdr());

                  if (!(emu_reg_ids[i] & 0x10))
                  {
                     read_emu(result, emu_reg_ids[i] & 0xF);
                  }

                  mapping[result] = emu_reg_ids[i] & 0xF;
                  usage_tag[result] = next_usage_tag ++;
                  weak[result] = (emu_reg_ids[i] & 0x10) ? true : false;

                  emu_reg_ids[i] = result;
                  found[i] = true;
               }
            }
         }
      }

      void mark_dirty(uint32_t native_reg)
      {
         dirty[native_reg] = true;
         weak[native_reg] = false;
      }

      void flush(psp_gpr_t native_reg)
      {
         if (dirty[native_reg] && !weak[native_reg])
         {
            write_emu(native_reg, mapping[native_reg]);
            dirty[native_reg] = false;
         }
      }

      void flush_all()
      {
         for (int i = psp_s0; i != psp_s5; i ++)
         {
            //if (is_usable(i))
            {
               flush((psp_gpr_t)i);
            }
         }
      }

   private:
      void read_emu(psp_gpr_t native, int emu)
      {
        loadReg(native, emu);
      }

      void write_emu(psp_gpr_t native, int emu)
      {
        storeReg(native, emu);
      }

   private:
      uint32_t mapping[30]; // Mapping[native] = emu
      uint32_t usage_tag[30];
      bool dirty[30];
      bool weak[30];

      uint32_t next_usage_tag;
};
