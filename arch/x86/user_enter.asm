; arch/x86/user_enter.asm
;
; void x86_enter_user_mode(uint32_t user_eip, uint32_t user_esp);

BITS 32

global x86_enter_user_mode

x86_enter_user_mode:
    mov eax, [esp + 4]
    mov edx, [esp + 8]

    mov cx, 0x23
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push dword 0x23
    push edx

    pushfd
    pop ecx
    or ecx, 0x00000200
    push ecx

    push dword 0x1B
    push eax

    iretd
