; arch/x86/gdt_flush.asm
;
; Loads the GDT and reloads all segment registers.
;
; C prototype:
;   void gdt_flush(uint32_t gdt_pointer_addr);

BITS 32

global gdt_flush
global tss_flush

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    
    ; 0x10 is our kernel data selector
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Reloading CS requires a far jump.
    ; 0x08 is our kernel code selector.
    jmp 0x08:.reload_cs

.reload_cs:
    ret

tss_flush:
    mov ax, [esp + 4]
    ltr ax
    ret
