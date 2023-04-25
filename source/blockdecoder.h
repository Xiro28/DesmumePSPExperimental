#include "types.h"

#include "arm_jit.h"
#include "instructions.h"
#include "instruction_attributes.h"
#include "armcpu.h"

#include <list>

#define _REG_NUM(i, n)		((i>>(n))&0x7)

#define RCPU   psp_k0
#define RCYC   psp_s0

static uint32_t block_procnum;

#define _ARMPROC (block_procnum ? NDS_ARM7 : NDS_ARM9)

#define _cond_table(x) arm_cond_table[x]

#define _cp15(x) ((u32)(((u8*)&cp15.x) - ((u8*)&_ARMPROC)))
#define _MMU(x) ((u32)(((u8*)&MMU.x) - ((u8*)&_ARMPROC)))
#define _NDS_ARM9(x) ((u32)(((u8*)&NDS_ARM9.x) - ((u8*)&_ARMPROC)))
#define _NDS_ARM7(x) ((u32)(((u8*)&NDS_ARM7.x) - ((u8*)&_ARMPROC)))

#define _reg(x) ((u32)(((u8*)&_ARMPROC.R[x]) - ((u8*)&_ARMPROC)))
#define _reg_pos(x) ((u32)(((u8*)&_ARMPROC.R[REG_POS(i,x)]) - ((u8*)&_ARMPROC)))
#define _thumb_reg_pos(x) ((u32)(((u8*)&_ARMPROC.R[_REG_NUM(i,x)]) - ((u8*)&_ARMPROC)))

#define _R15 _reg(15)

#define _instr_adr ((u32)(((u8*)&_ARMPROC.instruct_adr) - ((u8*)&_ARMPROC)))
#define _next_instr ((u32)(((u8*)&_ARMPROC.next_instruction) - ((u8*)&_ARMPROC)))
#define _instr ((u32)(((u8*)&_ARMPROC.instruction) - ((u8*)&_ARMPROC)))

#define main_mem ((u32)(((u8*)&_ARMPROC.MAIN_MEM[0]) - ((u8*)&_ARMPROC)))

#define _flags ((u32)(((u8*)&_ARMPROC.CPSR.val) - ((u8*)&_ARMPROC)))
#define _flag_N 31
#define _flag_Z 30
#define _flag_C 29
#define _flag_V 28
#define _flag_T  5

//LBU 
#define _flag_N8 7
#define _flag_Z8 6
#define _flag_C8 5
#define _flag_V8 4

#define conditional(x) \
    if (op.condition != -1 && !(op.extra_flags&EXTFL_MERGECOND)) {\
            conditional_label = emit_Halfbranch(op.condition);\
            jump_sz = emit_getCurrAdr();\
        }\
        {x;}\        
        if (conditional_label != 0 && op.condition != -1 && (op.extra_flags&EXTFL_SAVECOND)){\
            jump_sz = emit_getCurrAdr() - jump_sz;\
            CompleteCondition(op.condition, conditional_label, emit_getCurrAdr() + jump_sz + 8);\
        }

enum op{
    OP_NOP = 0,
    OP_ITP,
    OP_AND,
    OP_ORR,
    OP_BIC,
    OP_EOR,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_CLZ,
    OP_RSB,
    OP_NEG,
    OP_MOV,
    OP_MVN,

    OP_LDR = 128,
    OP_STR,
    OP_LDRH,
    OP_STRH,
    OP_STMIA,

    OP_LSR_0,

    OP_CMP = 512,
    OP_TST,
    OP_AND_S,
    OP_EOR_S,
    OP_ORR_S,
    OP_ADD_S,
    OP_SUB_S,
    OP_MOV_S,
    OP_MVN_S,
    OP_SWI
};

enum opType{
    PRE_OP_LSL_IMM,
    PRE_OP_LSL_REG,
    PRE_OP_LSR_IMM,
    PRE_OP_LSR_REG,
    PRE_OP_ASR_IMM,
    PRE_OP_ASR_REG,

    PRE_OP_REG_OFF,
    PRE_OP_REG_PRE_P,
    PRE_OP_REG_POST_P,
    PRE_OP_REG_PRE_M,
    PRE_OP_REG_POST_M,    

    PRE_OP_IMM,
    PRE_OP_PRE_P,
    PRE_OP_POST_P,
    PRE_OP_PRE_M,
    PRE_OP_POST_M,

    PRE_OP_NONE,
    NOFLAGS
};


enum extraFlags {
    EXTFL_NONE = 1,
    EXTFL_MERGECOND = 2,
    EXTFL_SAVECOND = 4,
    EXTFL_SKIPLOADFLAG = 8,
    EXTFL_SKIPSAVEFLAG = 16,
    EXTFL_DIRECTMEMACCESS = 32,
    EXTFL_NOFLAGS = 64
};

struct opcode{
    uint32_t rd;
    uint32_t rs1;
    uint32_t rs2;
    uint32_t imm;
    uint32_t op_pc;
    uint32_t condition;
    uint32_t extra_flags;

    op _op;
    opType preOpType;

    opcode(op opcode, uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t imm, opType preOpType, uint32_t pc, uint32_t condition = -1, uint32_t extra_flags = EXTFL_SAVECOND){
        this->_op = opcode;
        this->rd = rd;
        this->rs1 = rs1;
        this->rs2 = rs2;
        this->imm = imm;
        this->preOpType = preOpType;
        this->op_pc = pc;
        this->condition = condition;
        this->extra_flags = extra_flags|EXTFL_SAVECOND;
    }
};

class block{
    public:

        void init(){
            clearBlock();
            printf("block created\n");
        }

        void addOP(op _op, uint32_t pc, uint32 rd = -1, uint32 rs1 = -1, uint32 rs2 = -1, uint32 imm = -1, opType preOpType = PRE_OP_NONE, uint32_t condition = -1, uint32_t extra_flags = EXTFL_SAVECOND){
            
            if ((_op >= OP_CMP && _op != OP_SWI) || condition != -1) uses_flags = true;

            opcodes.push_back(opcode(_op, rd, rs1, rs2, imm, preOpType, pc, condition, extra_flags));

            if (_op == OP_ITP || _op == OP_SWI) return;
            
            onlyITP = false;

            if (rd  < 16) reg_usage[rd  + 1] += 1;
            if (rs1 < 16) reg_usage[rs1 + 1] += 1;
            if (rs2 < 16) reg_usage[rs2 + 1] += 1;
        }

        void clearBlock(){

            block_hash[0] = '\0';

            onlyITP = true;
            
            for (int i = 0; i < 16; ++i){
                reg_usage[i + 1] = 0;
            }

            if (opcodes.size() > 0) opcodes.clear();
        }

        u32 getNOpcodes() { return opcodes.size(); }

        template<int PROCNUM>
        bool emitArmBlock();

        template<int PROCNUM>
        bool emitThumbBlock();

        template<int PROCNUM>
        void emitArmBranch();    

        void optimize_basicblock();
        void optimize_basicblockThumb();

        bool noReadWriteOP = true;
        bool uses_flags = false;
        bool manualPrefetch = false;
        bool onlyITP = true;

        u32 branch_addr = 0;
        u32 start_addr = 0;

        char block_hash[1024];

    private:
        uint32 startAddr;
        uint32 endAddr;
        std::vector<opcode> opcodes;
        uint32_t reg_usage[17] {0}; //-1 register are stored in 0 so the actual registers start at 1
        uint32 getStartAddr();
        uint32 getEndAddr();
        void addOpcode(opcode op);
        void printBlock();
};

void emit_prefetch(const u8 isize, bool saveR15, bool is_ITP);

extern block currentBlock;

#define loadThumbReg(psp_reg, nds_reg) emit_lw(psp_reg, RCPU, _reg(nds_reg))
#define storeThumbReg(psp_reg, nds_reg) emit_sw(psp_reg, RCPU, _reg(nds_reg))

#define storeHalfReg(psp_reg, nds_reg) emit_sh(psp_reg, RCPU, _reg(nds_reg))