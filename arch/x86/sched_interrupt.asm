; arch/x86/sched_interrupt.asm
;
; Software scheduling interrupt for cooperative yield.

BITS 32

extern schedule_interrupt_handler

global sched_interrupt_stub

sched_interrupt_stub:
	push dword 0
	push dword 48
	jmp sched_common_stub

sched_common_stub:
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
	call schedule_interrupt_handler
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
