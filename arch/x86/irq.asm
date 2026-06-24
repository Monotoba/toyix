; arch/x86/irq.asm
;
; Hardware IRQ stubs for remapped PIC vectors 32..47.
;
; IRQ	-> vector 32
; IRQ	-> vector 33
; ...
; IRQ15	-> vector 47
;
; Hardware IRQs do not push CPU error codes, so every IRQ stub pushes
; a fake error code of 0, followed by the interrupt vector number.

BITS 32


extern irq_handler


%macro IRQ_STUB 2
global irq%1
irq%1:
	push dword 0
	push dword %2
	jmp irq_common_stub
%endmacro


IRQ_STUB 0,	32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47


global irq_common_stub
irq_common_stub:
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
	call irq_handler
	add esp, 4

	test eax, eax
	jz .restore_current
	mov esp, eax

.restore_current:
	
	pop eax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	
	popa
	
	add esp, 8
	
	iretd
	








