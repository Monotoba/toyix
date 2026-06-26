// arch/x86/gdt.h
#ifndef TOYIX_ARCH_X86_GDT_H
#define TOYIX_ARCH_X86_GDT_H

#include <stdint.h>

#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_KERNEL_DATA_SELECTOR 0x10u

#define X86_USER_CODE_SELECTOR   0x1Bu
#define X86_USER_DATA_SELECTOR   0x23u

#define X86_TSS_SELECTOR         0x28u

void gdt_init(void);

void tss_set_kernel_stack(uint32_t esp0);

#endif
