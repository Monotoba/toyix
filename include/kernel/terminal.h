#ifndef TOYIX_KERNEL_TERMINAL_H
#define TOYIX_KERNEL_TERMINAL_H

#include <stddef.h>

void terminal_init(void);

size_t terminal_readline(char *buffer, size_t buffer_size);

void terminal_test_once(void);

#endif
