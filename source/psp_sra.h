class reg_allocation
{
    uint32_t is_regAlloc[32]; // given nds register, returns the host one (or -1 if not allocated)
    uint32_t last_usagePC[32]; // given nds register, returns the last PC where it's used

    public:
        reg_allocation(){}

    void AllocReg(){
        
    }
};