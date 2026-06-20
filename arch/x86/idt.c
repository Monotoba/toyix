// arch/x86/idt.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "kernel/console.h"


typedef struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  always_zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed)) idt_entry_t;


typedef struct idt_pointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_pointer_t;


static idt_entry_t idt[256];
static idt_pointer_t idt_ptr;


extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);

extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);

extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);

extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);


static void idt_load(uint32_t idt_pointer_addr) {
    __asm__ volatile ("lidt (%0)" : : "r"(idt_pointer_addr));
}


static void idt_set_gate(
    uint8_t vector,
    uint32_t handler_addr,
    uint16_t selector,
    uint8_t flags
) {
    idt[vector].base_low = (uint16_t) (handler_addr & 0xFFFFu);
    idt[vector].selector = selector;
    idt[vector].always_zero = 0;
    idt[vector].flags = flags;
    idt[vector].base_high = (uint16_t)((handler_addr >> 16) & 0xFFFFu);
}


void idt_init(void) {
    void (*exception_stubs[32])(void) = {
        isr0,   isr1,   isr2,   isr3,
        isr4,   isr5,   isr6,   isr7,
        isr8,   isr9,   isr10,  isr11,
        isr12,  isr13,  isr14,  isr15,
        isr16,  isr17,  isr18,  isr19,
        isr20,  isr21,  isr22,  isr23,
        isr24,  isr25,  isr26,  isr27,
        isr28,  isr29,  isr30,  isr31
    };
    
    idt_ptr.limit = (uint16_t)(sizeof(idt) -1);
    idt_ptr.base = (uint32_t) &idt[0];
    
    for (uint16_t i = 0; i < 256; ++i) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }
    
    /*
     * 0xBE means:
     *  present
     *  descriptor privilage level 0
     *  32-bit interrupt gate
     */
    for (uint8_t i = 0; i < 32; ++i) {
        idt_set_gate(
            i,
            (uint32_t)exception_stubs[i],
            X86_KERNEL_CODE_SELECTOR,
            0x8Eu
        );
    }
    
    idt_load((uint32_t) & idt_ptr);
    
    console_writeln("IDT: installed CPU exception handlers");
}

