; arch/x86/paging_asm.asm
;
; Low-level paging control functions.
;
; C prototypes
;	void paging_load_directory(uint32_t physical_addr);
;	void paging_enable_asm(void);
;	uint32_t paging_read_cr0(void);
;	uint32_t paging_read_cr2(void);
;	uint32_t paging_read_cr3(void);

BITS 32

global paging_load_directory
global paging_enable_asm
global paging_read_cr0
global paging_read_cr2
global paging_read_cr3

paging_load_directory:
	mov eax, [esp + 4]
	mov cr3, eax
	ret
	
paging_enable_asm:
	mov eax, cr0
	or eax, 0x80000000	; CR0.PG
	mov cr0, eax
	
	; After enabling paging, execution continues at the next instruction.
	;Because we are identity-mapped, this address is still valid.
	ret
	
paging_read_cr0:
	mov eax, cr0
	ret
	
paging_read_cr2:
	mov eax, cr2
	ret
	
paging_read_cr3:
	mov eax, cr3
	ret







