
	.global _DisableFPUExceptions

    .set push 
    .set noreorder 
_DisableFPUExceptions: 
    cfc1    $2, $31 
    lui     $8, 0x80 
    and     $8, $2, $8     # Mask off all bits except for 23 of FCR 
    ctc1    $8, $31 
    jr      $31 
    nop 
    .set pop 


.global  set_op_logic_flags 		
.set noat 
.set noreorder

set_op_logic_flags:
    # N
    SRL  $8, $2, 31
    INS  $28, $8, 7, 1  

    # Z 
    SLTIU $8, $2, 1
    JR   $31           
    INS  $28, $8, 6, 1 
  

.global  set_sub_flags 		
.set noat  
.set noreorder

set_sub_flags: 

    # $28 = flags (4 = V, 5 = C, 6 = Z, 7 = N)
    # Corretto controllato con emulatore arm

    # C
    SLTU $t0, $a0, $a1    
    XORI $t0, $t0, 1    
    INS  $28, $t0, 5, 1   # 

    # Z 
    SLTIU $8, $v0, 1
    INS  $28, $8, 6, 1 

    # N
    SRL  $v1, $v0, 31
    INS  $28, $v1, 7, 1   

    # V           
    SLTU $8, $4, $5
    XOR $8, $8, $v1 
    JR   $31           
    INS  $28, $8, 4, 1   


.global  set_and_flags 		
.set noat 
.set noreorder

set_and_flags:
    SRL  $8, $2, 31
    INS  $28, $8, 7, 1

    SLTIU $8, $2, 1
    INS  $28, $8, 6, 1
     
    SLT $8, $4, $5
    JR $31
    INS  $28, $8, 4, 1