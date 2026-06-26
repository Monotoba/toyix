// kernel/toyexe.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/process.h"
#include "kernel/string.h"
#include "kernel/toyexe.h"

static uint32_t align_up_page(uint32_t value) {
    return (value + PMM_PAGE_SIZE - 1u) & ~(PMM_PAGE_SIZE - 1u);
}

static int validate_header(const toyexe_header_t *header, uint32_t file_size) {
    if (header == 0) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->magic != TOYEXE_MAGIC) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->version != TOYEXE_VERSION) {
        return TOYEXE_ERR_UNSUPPORTED;
    }

    if (header->header_size < sizeof(toyexe_header_t)) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->header_size > file_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset < header->header_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset > file_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_size == 0) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset + header->image_size < header->image_offset) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset + header->image_size > file_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->entry_offset >= header->image_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_size + header->bss_size < header->image_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_size + header->bss_size > 1024u * 1024u) {
        return TOYEXE_ERR_TOO_LARGE;
    }

    if (header->stack_size > 1024u * 1024u) {
        return TOYEXE_ERR_TOO_LARGE;
    }

    return TOYEXE_OK;
}

int toyexe_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
) {
    if (process == 0 || image == 0 || image_size < sizeof(toyexe_header_t)) {
        return TOYEXE_ERR_INVALID;
    }

    const toyexe_header_t *header = (const toyexe_header_t *)image;

    int rc = validate_header(header, image_size);

    if (rc != TOYEXE_OK) {
        return rc;
    }

    uint32_t runtime_size = header->image_size + header->bss_size;

    if (runtime_size == 0) {
        return TOYEXE_ERR_INVALID;
    }

    uint32_t stack_size = header->stack_size;

    if (stack_size == 0) {
        stack_size = TOYEXE_DEFAULT_STACK_SIZE;
    }

    stack_size = align_up_page(stack_size);

    uintptr_t user_base = TOYEXE_USER_BASE;
    uintptr_t user_stack_top = TOYEXE_DEFAULT_STACK_TOP;
    uintptr_t user_stack_base = user_stack_top - stack_size;

    if (process_map_user_region(process, user_base, runtime_size) != 0) {
        return TOYEXE_ERR_LOAD_FAILED;
    }

    if (process_map_user_region(process, user_stack_base, stack_size) != 0) {
        return TOYEXE_ERR_LOAD_FAILED;
    }

    const uint8_t *payload = image + header->image_offset;

    if (process_copy_to_user_init(
            process,
            user_base,
            payload,
            header->image_size
        ) != 0) {
        return TOYEXE_ERR_LOAD_FAILED;
    }

    if (header->bss_size != 0) {
        if (process_zero_user_init(
                process,
                user_base + header->image_size,
                header->bss_size
            ) != 0) {
            return TOYEXE_ERR_LOAD_FAILED;
        }
    }

    process->user_code_base = user_base;
    process_set_user_entry(process, user_base + header->entry_offset);
    process_set_user_stack(process, user_stack_base, user_stack_top);

    console_write("TOYEXE: loaded image bytes=");
    console_write_u32_dec(header->image_size);
    console_write(" bss=");
    console_write_u32_dec(header->bss_size);
    console_write(" entry=");
    console_write_hex32((uint32_t)(user_base + header->entry_offset));
    console_putc('\n');

    return TOYEXE_OK;
}

process_t *toyexe_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
) {
    process_t *process = process_create_empty(name);

    int rc = toyexe_load_process(process, image, image_size);

    if (rc != TOYEXE_OK) {
        console_write("TOYEXE: load failed rc=");
        console_write_u32_dec((uint32_t)(-rc));
        console_putc('\n');
        kernel_panic("TOYEXE process load failed");
    }

    process_start_user(process);

    return process;
}
