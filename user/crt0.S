.global _start
.extern main

.section .text.start, "ax"

_start:
    mov (%esp), %eax
    lea 4(%esp), %edx

    push %edx
    push %eax
    call main

    add $8, %esp

    mov %eax, %ebx
    mov $2, %eax
    int $0x80

.hang:
    jmp .hang
