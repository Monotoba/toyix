; arch/x86/boot.asm
;
; This file is the bridge between the bootloader and our C kernel.
;
; GRUB loads our ELF kernel, finds the Multiboot header, switches to the
; expected 32-bit environment, and jumps to _start.
;
; At entry:
;   EAX = Multiboot magic value
;   EBX = pointer to Multiboot information structure
;
; We create a stack, then call kernel_main(magic, info_ptr).

BITS 32

global _start
extern kernel_main

MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x00000003        ; bit 0: align modules, bit 1: request memory info
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

section .bss
align 16

stack_bottom:
    resb 16384                    ; 16 KiB bootstrap stack
stack_top:

section .text
align 16

_start:
    ; x86 stacks grow downward. Setting ESP to stack_top gives C a usable stack.
    mov esp, stack_top

    ; C uses cdecl on i386. Arguments are pushed right-to-left.
    ; kernel_main(uint32_t magic, uint32_t info_ptr)
    push ebx
    push eax
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
