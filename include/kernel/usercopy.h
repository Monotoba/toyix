// include/kernel/usercopy.h
#ifndef TOYIX_KERNEL_USERCOPY_H
#define TOYIX_KERNEL_USERCOPY_H

#include <stddef.h>
#include <stdint.h>

#define USERCOPY_OK 0
#define USERCOPY_ERR_FAULT -1
#define USERCOPY_ERR_TOO_LONG -2

int copy_from_user(void *kernel_dest, uintptr_t user_src, size_t size);
int copy_to_user(uintptr_t user_dest, const void *kernel_src, size_t size);

int user_string_length(
    uintptr_t user_str,
    size_t max_len,
    size_t *length_out
);

#endif
