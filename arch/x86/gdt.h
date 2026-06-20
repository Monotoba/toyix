// arch/x86/gdt.h
#ifndef TOYIX_ARCH_X86_GDT_H
#define TOYIX_ARCH_X86_GDT_H

#include <stdint.h>

#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_KERNEL_DATA_SELECTOR 0x10u

void gdt_init(void);

#endif
