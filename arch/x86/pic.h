// arch/x86/pic.h
#ifndef TOYIX_ARCH_X86_PIC_H
#define TOYIX_ARCH_X86_PIC_H


#include <stdint.h>


#define PIC_REMAP_MASTER_OFFSET 0x20u
#define PIC_REMAP_SLAVE_OFFSET 0x28u


void pic_init(void);
void pic_send_eoi(uint8_t irq_line);
void pic_set_mask(uint8_t irq_line);
void pic_clear_mask(uint8_t irq_line);


#endif
