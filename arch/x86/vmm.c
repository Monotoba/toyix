// arch/x86/vmm.c
#include <stdint.h>
#include "arch/x86/paging.h"
#include "arch/x86/vmm.h"
#include "include/kernel/console.h"
#include "include/kernel/panic.h"
#include "include/kernel/pmm.h"
#include "include/kernel/string.h"


#define PAGE_DIRECTORY_ENTRIES 	1024u
#define PAGE_TABLE_ENTRIES		1024u

/*
 * Page tables must be writable by the kernel while we are setting them up.
 *
 * Until the kernel maps all physical memory somewhere, we allocate page-table
 * pages below the original 16 MiB identity-mapped region.
 */
#define VMM_BOOTSTRAP_TABLE_LIMIT	0x01000000u


typedef uint32_t page_directory_entry_t;
typedef uint32_t page_table_entry_t;


static page_directory_entry_t *kernel_directory;
static uint32_t mapped_page_count;


extern void paging_invalidate_page(uintptr_t virtual_addr);
extern void paging_reload_cr3(void);


static int is_page_aligned(uintptr_t value) {
	return (value & (X86_PAGE_SIZE - 1u)) == 0;
}


static uint32_t directory_index(uintptr_t virtual_addr) {
	return (uint32_t)(virtual_addr >> 22);
}


static uint32_t table_index(uintptr_t virtual_addr) {
	return (uint32_t)((virtual_addr >> 12) & 0x3FFu);
}


static page_table_entry_t *table_from_pde(page_directory_entry_t pde) {
	uintptr_t physical = pde & X86_PAGE_FRAME_MASK;
	
	/*
	 * This cast is only currtently valid because page-table pages are allocated
	 * below 16 MiB, which Chapter 5 identity-mapped.
	 */
	return (page_table_entry_t *)physical;

}


static page_table_entry_t *get_or_create_page_table(
	uint32_t dir_index,
	uint32_t flags
) {
	page_directory_entry_t pde = kernel_directory[dir_index];
	
	if ((pde & X86_PAGE_PRESENT) != 0) {
		return table_from_pde(pde);
	}
	
	uintptr_t table_phys = pmm_alloc_page_below(VMM_BOOTSTRAP_TABLE_LIMIT);
	
	if (table_phys == PMM_INVALID_PAGE) {
		return 0;
	}
	
	
	page_table_entry_t *table = (page_table_entry_t *)table_phys;
	memset(table, 0, X86_PAGE_SIZE);
	
	uint32_t table_flags = 
		X86_PAGE_PRESENT |
		X86_PAGE_WRITABLE;
		
	if ((flags & X86_PAGE_USER) != 0) {
		table_flags |= X86_PAGE_USER;
	}
	
	kernel_directory[dir_index] =
		(table_phys & X86_PAGE_FRAME_MASK) | table_flags;
		
	/*
	 * The CPU may have cached the earlier non-present path. Reloading CR3 is
	 * conservative and simple for this early kernel.
	 */
	 paging_reload_cr3();
	 
	 return table;
	
}


void vmm_init(void) {
	kernel_directory = paging_get_kernel_directory();
	mapped_page_count = 0;
	
	if (kernel_directory == 0) {
		kernel_panic("vmm_init could not get kernel page directory");
	}
	
	console_writeln("VMM: initialized kernel address-space mapper");

}


int vmm_map_page(
	uintptr_t virtual_addr,
	uintptr_t physical_addr,
	uint32_t flags
) {
	if (!is_page_aligned(virtual_addr) || !is_page_aligned(physical_addr)) {
		return VMM_ERR_INVALID;
	}
	
	uint32_t dir = directory_index(virtual_addr);
	uint32_t tab = table_index(virtual_addr);
	
	page_table_entry_t *table = get_or_create_page_table(dir, flags);
	
	if ((table[tab] & X86_PAGE_PRESENT) != 0) {
		return VMM_ERR_ALREADY_MAPPED;
	}
	
	table[tab] = 
		(physical_addr & X86_PAGE_FRAME_MASK) |
		(flags & X86_PAGE_FLAGS_MASK) |
		X86_PAGE_PRESENT;
		
	paging_invalidate_page(virtual_addr);
	mapped_page_count++;
	
	return VMM_OK;
}


int vmm_unmap_page(uintptr_t virtual_addr) {
	if (!is_page_aligned(virtual_addr)) {
		return VMM_ERR_INVALID;
	}
	
	uint32_t dir = directory_index(virtual_addr);
	uint32_t tab = table_index(virtual_addr);
	
	page_directory_entry_t pde = kernel_directory[dir];
	
	if ((pde & X86_PAGE_PRESENT) == 0) {
		return VMM_ERR_NOT_MAPPED;
	}
	
	page_table_entry_t *table = table_from_pde(pde);
	
	if ((table[tab] & X86_PAGE_PRESENT) == 0) {
		return VMM_ERR_NOT_MAPPED;
	}
	
	table[tab] = 0;
	paging_invalidate_page(virtual_addr);
	
	if (mapped_page_count > 0) {
		mapped_page_count--;
	}
	
	return VMM_OK;
}


uintptr_t vmm_get_physical(uintptr_t virtual_addr) {
	uint32_t dir = directory_index(virtual_addr);
	uint32_t tab = table_index(virtual_addr);
	uint32_t offset = (uint32_t)(virtual_addr & (X86_PAGE_SIZE - 1u));
	
	page_directory_entry_t pde = kernel_directory[dir];
	
	if ((pde & X86_PAGE_PRESENT) == 0) {
		return 0;
	}
	
	page_table_entry_t *table = table_from_pde(pde);
	page_table_entry_t pte = table[tab];
	
	if ((pte & X86_PAGE_PRESENT) == 0) {
		return 0;
	}
	
	return (uintptr_t)((pte & X86_PAGE_FRAME_MASK) + offset);
}

void vmm_test_once(void) {
	console_writeln("VMM test: starting");
	
	uintptr_t physical = pmm_alloc_page();
	
	if (physical == PMM_INVALID_PAGE) {
		kernel_panic("VMM test could not allocated physical page");
	}
	
	/*
	 * Pick a high virtual page outside the 16 MiB identity map.
	 *
	 * We are not using 0xC0000000 yet because that address is commonly used
	 * later as the higher-half kernel base. This test address is temporary.
	 */
	uintptr_t virtual_addr = 0xD0000000u;
	
	int rc = vmm_map_page(
		virtual_addr,
		physical,
		X86_PAGE_WRITABLE
	);
	
	if (rc != VMM_OK) {
		kernel_panic("VMM test failed to map page");
	}
	
	uintptr_t translated = vmm_get_physical(virtual_addr);
	
	if (translated != physical) {
		kernel_panic("VMM test translation mismatch");
	}
	
	
	volatile uint32_t *mapped = (volatile uint32_t *)virtual_addr;
	
	mapped[0] = 0x11223344u;
	mapped[1] = 0x55667788u;
	
	if (mapped[0] != 0x11223344u || mapped[1] != 0x55667788u) {
		kernel_panic("VMM test mapped read/write failed");
	}
	
	rc = vmm_unmap_page(virtual_addr);
	
	if (rc != VMM_OK) {
		kernel_panic("VMM test failed to unmap page");
	}
	
	pmm_free_page(physical);
	
	console_writeln("VMM test: map/translate/write/unmap sanity check passed");
	
}
