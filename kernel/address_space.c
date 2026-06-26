// kernel/address_space.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/paging.h"
#include "kernel/address_space.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"

#define ADDRESS_SPACE_MAGIC 0x41535043u

#define PAGE_DIRECTORY_ENTRIES 1024u

#define PAGE_TABLE_BOOTSTRAP_LIMIT 0x01000000u

#define KERNEL_HIGH_BASE 0xC0000000u
#define LOW_IDENTITY_SHARED_BYTES 0x01000000u

static address_space_t kernel_space;
static address_space_t *current_space;

static uint32_t directory_index(uintptr_t virtual_addr) {
    return (uint32_t)((virtual_addr >> 22) & 0x3FFu);
}

static uint32_t table_index(uintptr_t virtual_addr) {
    return (uint32_t)((virtual_addr >> 12) & 0x3FFu);
}

static uint32_t to_arch_flags(uint32_t flags) {
    uint32_t arch_flags = 0;

    if ((flags & ADDRESS_SPACE_FLAG_WRITABLE) != 0) {
        arch_flags |= X86_PAGE_WRITABLE;
    }

    if ((flags & ADDRESS_SPACE_FLAG_USER) != 0) {
        arch_flags |= X86_PAGE_USER;
    }

    return arch_flags;
}

static uint32_t from_arch_flags(uint32_t arch_flags) {
    uint32_t flags = 0;

    if ((arch_flags & X86_PAGE_WRITABLE) != 0) {
        flags |= ADDRESS_SPACE_FLAG_WRITABLE;
    }

    if ((arch_flags & X86_PAGE_USER) != 0) {
        flags |= ADDRESS_SPACE_FLAG_USER;
    }

    return flags;
}

static void validate_space(address_space_t *space) {
    if (space == 0) {
        kernel_panic("address_space: null address space");
    }

    if (space->magic != ADDRESS_SPACE_MAGIC) {
        kernel_panic("address_space: magic mismatch");
    }

    if (space->page_directory == 0 ||
        space->page_directory_physical == 0) {
        kernel_panic("address_space: missing page directory");
    }
}

static int is_page_aligned(uintptr_t value) {
    return (value & (X86_PAGE_SIZE - 1u)) == 0;
}

static int user_virtual_allowed(uintptr_t virtual_addr) {
    return virtual_addr >= ADDRESS_SPACE_USER_BASE &&
           virtual_addr < ADDRESS_SPACE_USER_TOP;
}

static uint32_t *table_from_pde(uint32_t pde) {
    return (uint32_t *)(uintptr_t)(pde & X86_PAGE_FRAME_MASK);
}

static void sync_kernel_mappings(address_space_t *space) {
    uint32_t *kernel_dir = paging_get_kernel_directory();
    uint32_t *dir = space->page_directory;

    uint32_t low_shared_entries =
        LOW_IDENTITY_SHARED_BYTES / (4u * 1024u * 1024u);

    for (uint32_t i = 0; i < low_shared_entries; ++i) {
        dir[i] = kernel_dir[i];
    }

    uint32_t high_start = directory_index(KERNEL_HIGH_BASE);

    for (uint32_t i = high_start; i < PAGE_DIRECTORY_ENTRIES; ++i) {
        dir[i] = kernel_dir[i];
    }
}

void address_space_init(void) {
    kernel_space.magic = ADDRESS_SPACE_MAGIC;
    kernel_space.page_directory = paging_get_kernel_directory();
    kernel_space.page_directory_physical =
        (uintptr_t)paging_get_kernel_directory();
    kernel_space.user_page_count = 0;

    current_space = &kernel_space;

    console_writeln("Address space: kernel address space registered");
}

address_space_t *address_space_kernel(void) {
    return &kernel_space;
}

address_space_t *address_space_current(void) {
    return current_space;
}

address_space_t *address_space_create(void) {
    uintptr_t directory_phys =
        pmm_alloc_page_below(PAGE_TABLE_BOOTSTRAP_LIMIT);

    if (directory_phys == PMM_INVALID_PAGE) {
        kernel_panic("address_space_create could not allocate page directory");
    }

    uint32_t *directory = (uint32_t *)(uintptr_t)directory_phys;
    memset(directory, 0, X86_PAGE_SIZE);

    address_space_t *space =
        (address_space_t *)kmalloc(sizeof(address_space_t));

    if (space == 0) {
        kernel_panic("address_space_create could not allocate object");
    }

    space->magic = ADDRESS_SPACE_MAGIC;
    space->page_directory = directory;
    space->page_directory_physical = directory_phys;
    space->user_page_count = 0;

    sync_kernel_mappings(space);

    console_writeln("Address space: created process page directory");

    return space;
}

void address_space_switch(address_space_t *space) {
    validate_space(space);

    if (space != &kernel_space) {
        sync_kernel_mappings(space);
    }

    if (current_space == space) {
        return;
    }

    current_space = space;
    paging_load_directory((uint32_t)space->page_directory_physical);
}

static uint32_t *get_or_create_table(
    address_space_t *space,
    uint32_t dir_index,
    uint32_t flags
) {
    uint32_t *directory = space->page_directory;
    uint32_t pde = directory[dir_index];

    if ((pde & X86_PAGE_PRESENT) != 0) {
        return table_from_pde(pde);
    }

    uintptr_t table_phys =
        pmm_alloc_page_below(PAGE_TABLE_BOOTSTRAP_LIMIT);

    if (table_phys == PMM_INVALID_PAGE) {
        return 0;
    }

    uint32_t *table = (uint32_t *)(uintptr_t)table_phys;
    memset(table, 0, X86_PAGE_SIZE);

    uint32_t pde_flags =
        X86_PAGE_PRESENT |
        X86_PAGE_WRITABLE |
        to_arch_flags(flags);

    directory[dir_index] =
        (uint32_t)(table_phys & X86_PAGE_FRAME_MASK) | pde_flags;

    return table;
}

int address_space_map_page(
    address_space_t *space,
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
) {
    validate_space(space);

    if (!is_page_aligned(virtual_addr) ||
        !is_page_aligned(physical_addr)) {
        return -1;
    }

    if (!user_virtual_allowed(virtual_addr)) {
        return -1;
    }

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t *table = get_or_create_table(space, dir, flags);

    if (table == 0) {
        return -1;
    }

    if ((table[tab] & X86_PAGE_PRESENT) != 0) {
        return -1;
    }

    uint32_t pte_flags =
        X86_PAGE_PRESENT |
        to_arch_flags(flags);

    table[tab] =
        (uint32_t)(physical_addr & X86_PAGE_FRAME_MASK) | pte_flags;

    space->user_page_count++;

    if (space == current_space) {
        paging_invalidate_page(virtual_addr);
    }

    return 0;
}

int address_space_unmap_page(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    validate_space(space);

    if (!is_page_aligned(virtual_addr)) {
        return -1;
    }

    if (!user_virtual_allowed(virtual_addr)) {
        return -1;
    }

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t pde = space->page_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return -1;
    }

    uint32_t *table = table_from_pde(pde);

    if ((table[tab] & X86_PAGE_PRESENT) == 0) {
        return -1;
    }

    table[tab] = 0;

    if (space->user_page_count > 0) {
        space->user_page_count--;
    }

    if (space == current_space) {
        paging_invalidate_page(virtual_addr);
    }

    return 0;
}

uintptr_t address_space_get_physical(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    validate_space(space);

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t pde = space->page_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    uint32_t *table = table_from_pde(pde);
    uint32_t pte = table[tab];

    if ((pte & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    return (pte & X86_PAGE_FRAME_MASK) |
           (virtual_addr & (X86_PAGE_SIZE - 1u));
}

uint32_t address_space_get_flags(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    validate_space(space);

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t pde = space->page_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    uint32_t *table = table_from_pde(pde);
    uint32_t pte = table[tab];

    if ((pte & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    return from_arch_flags(pte & X86_PAGE_FLAGS_MASK);
}
