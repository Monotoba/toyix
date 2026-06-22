// arch/x86/paging.h
#ifndef TOYIX_ARCH_X86_PAGING_H
#define TOYIX_ARCH_X86_PAGING_H

#include <stdint.h>

#define X86_PAGE_SIZE 4096u

#define X86_PAGE_PRESENT  0x001u
#define X86_PAGE_WRITABLE 0x002u
#define X86_PAGE_USER	  0x004u


void paging_init(void);
int paging_is_enabled(void);
void paging_test_identity_mapping(void);


#endif
