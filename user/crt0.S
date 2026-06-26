.global _start
.extern main

.section .text.start, "ax"

_start:
    call main

    mov %eax, %ebx
    mov $2, %eax
    int $0x80

.hang:
    jmp .hang
