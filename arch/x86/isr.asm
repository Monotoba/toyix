; arch/x86/isr.asm
;
; Exception stub for CPU vectors 0..31.
;
; Some CPU exceptions push an error code automatically.
; Others do not.
;
; To make the C handler simple, every stub creates the same shape:
;
;   interrupt_number
;   error_code
;   CPU-pushed return frame
;
; Then isr_common_stub saves general registers and calls isr_handler().

BITS 32

extern isr_handler
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common_stub
%endmacro


%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common_stub
%endmacro


ISR_NOERR 0     ; Divide Error
ISR_NOERR 1     ; Debug
ISR_NOERR 2     ; Non-Maskable Interrupt
ISR_NOERR 3     ; Breakpoint
ISR_NOERR 4     ; Overflow
ISR_NOERR 5     ; Bound Range Exceeded
ISR_NOERR 6     ; Invalid Opcode
ISR_NOERR 7     ; Device Not Available
ISR_NOERR 8     ; Double Fault
ISR_NOERR 9     ; Coprocessor Segment Overrun / reserved on newer CPUs
ISR_ERR   10    ; Invalid TSS
ISR_ERR   11    ; Segment Not Present
ISR_ERR   12    ; Stack-Segment Fault
ISR_ERR   13    ; General Protection Fault
ISR_ERR   14    ; Page Fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; x87 Floating-Point Exception
ISR_ERR   17    ; Alignment Check
ISR_NOERR 18    ; Machine Check
ISR_NOERR 19    ; SIMD Floating-Point Exception
ISR_NOERR 20    ; Virtual Exception or reserved, depending on CPU
ISR_NOERR 21    ;
ISR_NOERR 22    ; 
ISR_NOERR 23    ;
ISR_NOERR 24    ;
ISR_NOERR 25    ;
ISR_NOERR 26    ;
ISR_NOERR 27    ;
ISR_NOERR 28    ;
ISR_NOERR 29    ;
ISR_NOERR 30    ;
ISR_NOERR 31    ;

global isr_common_stub
isr_common_stub:
    ; Save general-purpose registers.
    pusha
    
    ; Save current data segment selector.
    xor eax, eax    ; zero registers
    mov ax, ds
    push eax
    
    ; Load kernel data selector into data segment registers.
    mov ax,0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Pass pointer to interrupt_frame_t.
    push esp
    call isr_handler
    add esp, 4
    
    ; Restore original data segment selector.
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Restore general-purpose registers.
    popa
    
    ; Drop interrupt_number and error_code.
    add esp, 8
    
    iretd
    










