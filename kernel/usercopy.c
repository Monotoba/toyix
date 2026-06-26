// kernel/usercopy.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/address_space.h"
#include "kernel/string.h"
#include "kernel/usercopy.h"

static int user_range_accessible(uintptr_t user_addr, size_t size) {
    if (size == 0) {
        return 1;
    }

    if (user_addr + size < user_addr) {
        return 0;
    }

    uintptr_t start = user_addr;
    uintptr_t end = user_addr + size - 1u;
    uintptr_t page = start & ~(uintptr_t)0xFFFu;

    address_space_t *space = address_space_current();

    while (page <= end) {
        uint32_t flags = address_space_get_flags(space, page);

        if ((flags & ADDRESS_SPACE_FLAG_USER) == 0) {
            return 0;
        }

        if (page > 0xFFFFFFFFu - 0x1000u) {
            break;
        }

        page += 0x1000u;
    }

    return 1;
}

int copy_from_user(void *kernel_dest, uintptr_t user_src, size_t size) {
    if (kernel_dest == 0 && size != 0) {
        return USERCOPY_ERR_FAULT;
    }

    if (!user_range_accessible(user_src, size)) {
        return USERCOPY_ERR_FAULT;
    }

    memcpy(kernel_dest, (const void *)user_src, size);
    return USERCOPY_OK;
}

int copy_to_user(uintptr_t user_dest, const void *kernel_src, size_t size) {
    if (kernel_src == 0 && size != 0) {
        return USERCOPY_ERR_FAULT;
    }

    if (!user_range_accessible(user_dest, size)) {
        return USERCOPY_ERR_FAULT;
    }

    memcpy((void *)user_dest, kernel_src, size);
    return USERCOPY_OK;
}

int user_string_length(
    uintptr_t user_str,
    size_t max_len,
    size_t *length_out
) {
    if (length_out == 0) {
        return USERCOPY_ERR_FAULT;
    }

    for (size_t i = 0; i < max_len; ++i) {
        char ch;

        if (copy_from_user(&ch, user_str + i, 1) != USERCOPY_OK) {
            return USERCOPY_ERR_FAULT;
        }

        if (ch == '\0') {
            *length_out = i;
            return USERCOPY_OK;
        }
    }

    return USERCOPY_ERR_TOO_LONG;
}
