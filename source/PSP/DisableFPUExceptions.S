
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
    INS  $27, $8, 7, 1  

    # Z 
    SLTIU $8, $2, 1
    JR   $31           
    INS  $27, $8, 6, 1 
  

.global  set_sub_flags 		
.set noat  
.set noreorder

set_sub_flags: 

    # $27 = flags (4 = V, 5 = C, 6 = Z, 7 = N)

    # C
    SLTU $8, $4, $5     
    XORI $8, $8, 1      
    INS  $27, $8, 5, 1   

    # N
    SRL  $12, $2, 31
    INS  $27, $12, 7, 1  

    # Z 
    SLTIU $8, $2, 1
    INS  $27, $8, 6, 1   

    # V
    SLT  $10, $4, $5   
    XOR  $8, $12, $10
    JR   $31           
    INS  $27, $8, 4, 1   


.global  set_and_flags 		
.set noat 
.set noreorder

set_and_flags:
    SRL  $8, $2, 31
    INS  $27, $8, 7, 1

    SLTIU $8, $2, 1
    INS  $27, $8, 6, 1
     
    SLT $8, $4, $5
    JR $31
    INS  $27, $8, 4, 1