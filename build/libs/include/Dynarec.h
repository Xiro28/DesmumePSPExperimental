#ifndef Dyanrec_H
#define Dynarec_H

#include <malloc.h>
#include <cstring>
#include <pspsuspend.h>

enum REG_MIPS{
    _None = -1,
    _zero = 0,
    _at, 
    _v0, _v1,
    _a0, _a1, _a2, _a3,
    _t0, _t1, _t2, _t3,_t4, _t5, _t6, _t7,
    _s0, _s1, _s2, 
    
    // FLAG STUFF (DESMUME)
    //      s3 -> Q
    //      s4 -> N 
    //      s5 -> Z 
    //      s6 -> C 
    //      s7 -> V 
            _s3, _s4, _s5, _s6, _s7,

    _t8, _t9
};

enum Type_J{
      _j = 0b000010,
    _jal = 0b000011
};

enum Type_I{
     _li  = 0b00100100,
     _lui = 0b00001111,
     //_lw  = 0b00100011,

     _ori = 0b00001101,

    _mfhi = 0b00010000,
    _mflo = 0b00010010,

    _mult = 0b00011000,

      __sw = 0b00101011
};

enum Type_R{
    _add  = 0b00000100000,
    _sub  = 0b00000100010,
    _div  = 0b00000011010,

    _and  = 0b00000100100,
     _or  = 0b00000100011,
    _xor  = 0b00000100110,
    _not  = 0b00000100111,

     _jr  = 0b00000001000,
   _jalr  = 0b00000001001,

    _sll  = 0b00000000000,
    _sllv = 0b00000000100,

    _srl  = 0b00000000010,
    _srlv = 0b00000000110,

    _move = 0b00000100001,
    _movn = 0b00000001011,
    _movz = 0b00000001010
};

class PSPDynarec{

private:
    int MAX_SZ = 16 * 1024 * 256; //3.75 MB
    __attribute__((aligned(64))) uint8_t * code;

public:
    uint64_t  count;

    void Init(){
        int ret = sceKernelVolatileMemLock(0, reinterpret_cast<void**>(&code), &MAX_SZ); 
        count = 0;

        memset(code,0,MAX_SZ);

        if (ret != 0) printf("Failed to allocate volatile mem ");
        else          printf("Volatile mem allocated correctly");
    }

    void   GetValueFromReg(REG_MIPS reg,uint64_t &value);
    u32    ExecuteFromAddr(u32 addr); 

    void*       GetMemPoint(uint64_t addr)  { return &code[addr]; }  
    uint64_t    GetMemPos()                 { return count; } 
    void        SetMemPos(uint64_t pos)     { count = pos; }   
    void        ClearMembuffer()            { memset(code,0,MAX_SZ); }   
    void        DeInit()                    { ClearMembuffer(); sceKernelVolatileMemUnlock(0); }
    
};

class PSPD_Fun{
    private:
        bool writePending = false;

        uint8_t flag;

        uint16_t codePointer;
        uint64_t  memPosition;
        uint8_t* code;
        PSPDynarec* _dynarec;        
        
        int instructionPending = -1;

        void EmitBYTE(u8 byte);
        void EmitWORD(u16 word);
        void EmitDWORD(u32 dword);

    public:
        //TODO Fix: This code will execute a nop as first operation. 
        PSPD_Fun(PSPDynarec *dynarec,uint64_t addr){
            memPosition =  addr;
            code = reinterpret_cast<uint8_t*>(dynarec->GetMemPoint(dynarec->count)); 
            codePointer = dynarec->GetMemPos()/8;
            _dynarec = dynarec;
        }

        void PrintInstrCount();
        u32 Finalize();
        u32 Execute();

        void customOP(int _code){   }
        
        template<Type_J _op>                                     void OP_J(int addr);

        template<Type_I _op,REG_MIPS rs>                         void OP_I();
        template<Type_I _op,REG_MIPS rs, REG_MIPS rt>            void OP_I();
        template<Type_I _op,REG_MIPS rs>                         void OP_I(uint32_t value);
        template<Type_I _op,REG_MIPS rs, REG_MIPS rt>            void OP_I(int addr);

        template<Type_R _op,REG_MIPS rs,REG_MIPS rd>             void OP_R(uint64_t value);
        template<Type_R _op,REG_MIPS rs>                         void OP_R(uint8_t  sa);
        template<Type_R _op,REG_MIPS rs,REG_MIPS rd,REG_MIPS rt> void OP_R();

};

template<Type_R _op,REG_MIPS rs,REG_MIPS rd,REG_MIPS rt> 
void PSPD_Fun::OP_R()
{
     u32 instruction = _op;
     instruction |= (int)rs<<11;
     instruction |= (int)rd<<21;
     instruction |= (int)rt<<16;
     if (instructionPending != -1) EmitDWORD(instructionPending); 
     instructionPending = instruction;   
}

template<Type_R _op,REG_MIPS rs,REG_MIPS rd> 
void PSPD_Fun::OP_R(uint64_t value)
{    
    u32 instruction = _op;
    instruction |= (int)rs<<11;
    instruction |= (int)rd<<16;
    instruction |= static_cast<uint8_t>(value<<21);
    if (instructionPending != -1) EmitDWORD(instructionPending); 
    instructionPending = instruction;
}

template<Type_R _op,REG_MIPS rs> 
void PSPD_Fun::OP_R(uint8_t sa)
{    
    u32 instruction = _op;
    instruction |=      sa<<6;
    instruction |= (int)rs<<11;
    instruction |= (int)rs<<16;
    if (instructionPending != -1) EmitDWORD(instructionPending); 
    instructionPending = instruction;
}

template<Type_I _op,REG_MIPS rs, REG_MIPS rd> 
void PSPD_Fun::OP_I(int addr){
    u32 instruction = addr;
   
    if (rs != -1) {
        instruction |= rd << 21;
        instruction |= rs << 16;
    }else
        instruction |= rd << 16;

    instruction |= _op << 26;

    EmitDWORD(instruction);
}

template<Type_I _op,REG_MIPS rs,REG_MIPS rd> 
void PSPD_Fun::OP_I(){
     u32 instruction = _op;                        
     instruction |= (int)rs<<21;
     instruction |= (int)rd<<16;                 
     if (instructionPending != -1) EmitDWORD(instructionPending); 
     instructionPending = instruction;
}
      
template<Type_I _op,REG_MIPS rs> 
void PSPD_Fun::OP_I(uint32_t value){
    EmitWORD(value);
    EmitBYTE(rs);
    EmitBYTE(_op);
}

template<Type_I _op,REG_MIPS rs> 
void PSPD_Fun::OP_I(){
    u32 instruction = _op;
    instruction |= (int)rs<<11;
    if (instructionPending != -1) EmitDWORD(instructionPending); 
    instructionPending = instruction;
}
                                                                                                          
template<Type_J _op> 
void PSPD_Fun::OP_J(int addr){
    u32 instruction = (addr>>2);
    instruction |= _op << 26;
    EmitDWORD(instruction);
    EmitDWORD(0); //Add a nop after the jump 
}                                              

#endif
