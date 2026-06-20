// arch/x86/interrupts.h
#ifndef TOYIX_ARCH_X86_INTERRUPTS_H
#define TOYIX_ARCH_X86_INTERRUPTS_H

#include <stdint.h>

typedef struct interrupt_frame {
    uint32_t ds;
    
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t original_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    
    uint32_t interrupt_number;
    uint32_t error_code;
    
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} interrupt_frame_t;


void isr_handler(interrupt_frame_t *frame);


#endif
