// include/kernel/toyexe.h
#ifndef TOYIX_KERNEL_TOYEXE_H
#define TOYIX_KERNEL_TOYEXE_H

#include <stdint.h>
#include "kernel/process.h"

#define TOYEXE_MAGIC   0x45595854u
#define TOYEXE_VERSION 1u

#define TOYEXE_USER_BASE         0x40100000u
#define TOYEXE_DEFAULT_STACK_TOP 0x70000000u
#define TOYEXE_DEFAULT_STACK_SIZE 4096u

#define TOYEXE_OK 0
#define TOYEXE_ERR_INVALID -1
#define TOYEXE_ERR_UNSUPPORTED -2
#define TOYEXE_ERR_TOO_LARGE -3
#define TOYEXE_ERR_LOAD_FAILED -4

typedef struct toyexe_header {
    uint32_t magic;
    uint32_t version;

    uint32_t header_size;

    uint32_t entry_offset;

    uint32_t image_offset;
    uint32_t image_size;

    uint32_t bss_size;
    uint32_t stack_size;
} __attribute__((packed)) toyexe_header_t;

int toyexe_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
);

process_t *toyexe_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);

#endif
