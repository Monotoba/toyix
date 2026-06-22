// arch/x86/interrupts.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/pic.h"
#include "kernel/console.h"
#include "kernel/panic.h"


static interrupt_handler_t interrupt_handlers[256];


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



int interrupt_register_handler(uint8_t vector, interrupt_handler_t handler) {
	interrupt_handlers[vector] = handler;
	return 0;
}


void interrupts_enable(void) {
	__asm__ volatile ("sti");
}


void interrupts_disable(void) {
	__asm__ volatile ("cli");
}


void interrupts_wait(void) {
	__asm__ volatile ("hlt");
} 


static void print_register(const char *name, uint32_t value) {
    console_write(name);
    console_write("=");
    console_write_hex32(value);
    console_putc(' ');
}


void isr_handler(interrupt_frame_t *frame) {
    if (interrupt_handlers[frame->interrupt_number] != NULL) {
        interrupt_handlers[frame->interrupt_number](frame);
        return;
    }
	
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


void irq_handler(interrupt_frame_t *frame) {
	if (frame == NULL) {
		kernel_panic("null IRQ frame");
	}
	
	uint32_t vector = frame->interrupt_number;
	
	if (vector >= 32 && vector <= 47) {
		if (interrupt_handlers[vector] != NULL) {
			interrupt_handlers[vector] (frame);
		}
		
		pic_send_eoi((uint8_t) (vector - 32));
		return;
	}
	
	console_write("Unexpected IRQ vector ");
	console_write_hex32(vector);
	console_putc('\n');
}
