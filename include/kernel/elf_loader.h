// include/kernel/elf_loader.h
#ifndef TOYIX_KERNEL_ELF_LOADER_H
#define TOYIX_KERNEL_ELF_LOADER_H

#include <stdint.h>
#include "kernel/process.h"

#define ELF_LOADER_OK 0
#define ELF_LOADER_ERR_INVALID -1
#define ELF_LOADER_ERR_UNSUPPORTED -2
#define ELF_LOADER_ERR_TOO_LARGE -3
#define ELF_LOADER_ERR_LOAD_FAILED -4

#define ELF_USER_STACK_TOP  0x70000000u
#define ELF_USER_STACK_SIZE 4096u

int elf_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
);

process_t *elf_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);

#endif
