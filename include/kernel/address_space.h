// include/kernel/address_space.h
#ifndef TOYIX_KERNEL_ADDRESS_SPACE_H
#define TOYIX_KERNEL_ADDRESS_SPACE_H

#include <stdint.h>

#define ADDRESS_SPACE_FLAG_WRITABLE 0x00000001u
#define ADDRESS_SPACE_FLAG_USER     0x00000002u

#define ADDRESS_SPACE_USER_BASE 0x01000000u
#define ADDRESS_SPACE_USER_TOP  0xC0000000u

typedef struct address_mapping {
    uintptr_t virtual_addr;
    uintptr_t physical_addr;

    struct address_mapping *next;
} address_mapping_t;

typedef struct address_space {
    uint32_t magic;

    uint32_t *page_directory;
    uintptr_t page_directory_physical;

    address_mapping_t *user_mappings;

    uint32_t user_page_count;
} address_space_t;

void address_space_init(void);

address_space_t *address_space_kernel(void);
address_space_t *address_space_current(void);

address_space_t *address_space_create(void);
void address_space_destroy(address_space_t *space);

void address_space_switch(address_space_t *space);

int address_space_map_page(
    address_space_t *space,
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
);

int address_space_unmap_page(
    address_space_t *space,
    uintptr_t virtual_addr
);

uintptr_t address_space_get_physical(
    address_space_t *space,
    uintptr_t virtual_addr
);

uint32_t address_space_get_flags(
    address_space_t *space,
    uintptr_t virtual_addr
);

#endif
