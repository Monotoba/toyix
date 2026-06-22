// include/kernel/pmm.h
#ifndef TOYIX_KERNEL_PMM_H
#define TOYIX_KERNEL_PMM_H

#include <stdint.h>
#include "arch/x86/multiboot.h"

#define PMM_PAGE_SIZE 4096u
#define PMM_INVALID_PAGE 0u


typedef struct pmm_stats {
	uint32_t total_frames;
	uint32_t usable_frames;
	uint32_t reserved_frames;
	uint32_t free_frames;
	uint32_t used_frames;
	uint32_t highest_physical_addr;
} pmm_stats_t;

void pmm_init(const multiboot_info_t * mbi);

uintptr_t pmm_alloc_page(void);
void pmm_free_page(uintptr_t physical_addr);

pmm_stats_t pmm_get_stats(void);
void pmm_dump_stats(void);
void pmm_test_once(void);


#endif
