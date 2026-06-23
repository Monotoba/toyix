// arch/x86/paging.h
#ifndef TOYIX_ARCH_X86_PAGING_H
#define TOYIX_ARCH_X86_PAGING_H

#include <stdint.h>

#define X86_PAGE_SIZE 4096u

#define X86_PAGE_PRESENT  0x001u
#define X86_PAGE_WRITABLE 0x002u
#define X86_PAGE_USER	  0x004u


#define X86_PAGE_WRITETHROUGH	0x008u
#define X86_PAGE_NOCACHE		0x010u
#define X86_PAGE_ACCESSED		0x020u
#define X86_PAGE_DIRTY			0x040u
#define X86_PAGE_GLOBAL			0x100u

#define X86_PAGE_FRAME_MASK		0xFFFFF000u
#define X86_PAGE_FLAGS_MASK		0x00000FFFu


void paging_init(void);
int paging_is_enabled(void);
void paging_test_identity_mapping(void);


uint32_t *paging_get_kernel_directory(void);


#endif
