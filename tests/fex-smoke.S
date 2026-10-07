.global _start
.section .text
_start:
    xor %eax, %eax
    mov $1000, %ecx
1:
    add %ecx, %eax
    loop 1b
    cmp $500500, %eax
    jne failed
    mov $1, %eax
    mov $1, %edi
    lea message(%rip), %rsi
    mov $message_end-message, %edx
    syscall
    xor %edi, %edi
    jmp exit
failed:
    mov $1, %edi
exit:
    mov $60, %eax
    syscall
.section .rodata
message:
    .ascii "ARMADA_VR_FEX_PASS\n"
message_end:
.section .note.GNU-stack,"",@progbits
