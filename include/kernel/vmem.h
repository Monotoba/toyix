// include/kernel/vmem.h
#ifndef TOYIX_KERNEL_VMEM_H
#define TOYIX_KERNEL_VMEM_H

#include <stdint.h>

#define VMEM_OK 				0
#define VMEM_ERR_INVALID 		-1
#define VMEM_ERR_NO_MEMORY 		-2
#define VMEM_ERR_ALREADY_MAPPED	-3
#define VMEM_ERR_NOT_MAPPED		-4

#define VMEM_FLAG_WRITABLE	0x00000001u
#define VMEM_FLAG_USER		0x00000002u


void vmem_init(void);


int vmem_map_page (
	uintptr_t virtual_addr,
	uintptr_t physical_addr,
	uint32_t flags
);


int vmem_unmap_page(uintptr_t virtual_addr);


uintptr_t vmem_get_physical(uintptr_t virtual_addr);

void vmem_test_once(void);


#endif

