// arch/x86/vmm.h
#ifndef TOYIX_ARCH_X86_VMM_H
#define TOYIX_ARCH_X86_VMM_H

#include <stdint.h>

#define VMM_OK 0
#define VMM_ERR_INVALID -1
#define VMM_ERR_NO_MEMORY -2
#define VMM_ERR_ALREADY_MAPPED -3
#define VMM_ERR_NOT_MAPPED -4


void vmm_init(void);


int vmm_map_page(
	uintptr_t virtual_addr,
	uintptr_t physical_addr,
	uint32_t flags
);


int vmm_unmap_page(uintptr_t virtual_addr);


uintptr_t vmm_get_physical(uintptr_t virtual_addr);


void vmm_test_once(void);

#endif

