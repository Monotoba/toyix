// kernel/vmem.c
#include <stdint.h>
#include "arch/x86/paging.h"
#include "arch/x86/vmm.h"
#include "kernel/vmem.h"

static uint32_t vmem_to_arch_flags(uint32_t flags) {
	uint32_t arch_flags = 0;

	if ((flags & VMEM_FLAG_WRITABLE) != 0) {
		arch_flags |= X86_PAGE_WRITABLE;
	}

	if ((flags & VMEM_FLAG_USER) != 0) {
		arch_flags |= X86_PAGE_USER;
	}

	return arch_flags;
}

static int vmem_from_arch_result(int result) {
	switch (result) {
	case VMM_OK:
		return VMEM_OK;
	case VMM_ERR_INVALID:
		return VMEM_ERR_INVALID;
	case VMM_ERR_NO_MEMORY:
		return VMEM_ERR_NO_MEMORY;
	case VMM_ERR_ALREADY_MAPPED:
		return VMEM_ERR_ALREADY_MAPPED;
	case VMM_ERR_NOT_MAPPED:
		return VMEM_ERR_NOT_MAPPED;
	default:
		return VMEM_ERR_INVALID;
	}
}

void vmem_init(void) {
	vmm_init();
}

int vmem_map_page(
	uintptr_t virtual_addr,
	uintptr_t physical_addr,
	uint32_t flags
) {
	return vmem_from_arch_result(
		vmm_map_page(
			virtual_addr,
			physical_addr,
			vmem_to_arch_flags(flags)
		)
	);
}

int vmem_unmap_page(uintptr_t virtual_addr) {
	return vmem_from_arch_result(vmm_unmap_page(virtual_addr));
}

uintptr_t vmem_get_physical(uintptr_t virtual_addr) {
	return vmm_get_physical(virtual_addr);
}

void vmem_test_once(void) {
	vmm_test_once();
}
