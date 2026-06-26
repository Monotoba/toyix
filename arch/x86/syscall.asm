; arch/x86/syscall.asm
;
; int 0x80 syscall entry.

BITS 32

extern syscall_handler

global syscall_stub

syscall_stub:
    push dword 0
    push dword 128

    pusha

    xor eax, eax
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call syscall_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa

    add esp, 8

    iretd
