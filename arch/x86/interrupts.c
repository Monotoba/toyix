// arch/x86/interrupts.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "kernel/console.h"
#include "kernel/panic.h"


static const char *const exception_names[32] = {
   "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};


static void print_register(const char *name, uint32_t value) {
    console_write(name);
    console_write("=");
    console_write_hex32(value);
    console_putc(' ');
}


void isr_handler(interrupt_frame_t *frame) {
    console_writeln("");
    console_writeln("*** CPU EXCEPTION ***");
    
    console_write("Vector: ");
    console_write_hex32(frame->interrupt_number);
    
    if (frame->interrupt_number < 32) {
        console_write(" (");
        console_write(exception_names[frame->interrupt_number]);
        console_write(")");
    }
    
    console_putc('\n');
    console_write_hex32(frame->error_code);
    console_putc('\n');
    
    print_register("EAX", frame->eax);
    print_register("EBX", frame->ebx);
    print_register("ECX", frame->ecx);
    print_register("EDX", frame->edx);
    console_putc('\n');
    
    print_register("ESI", frame->esi);
    print_register("EDI", frame->edi);
    print_register("EBP", frame->ebp);
    console_putc('\n');
    
    print_register("EIP", frame->eip);
    print_register("CS", frame->cs);
    print_register("EFLAGS", frame->eflags);
    console_putc('\n');
    
    if (frame->interrupt_number == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        
        console_write("Page fault address CR2=");
        console_write_hex32(cr2);
        console_putc('\n');
    }
    
    kernel_panic("unhandled CPU exception");
}
