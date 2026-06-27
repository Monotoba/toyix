#ifndef TOYIX_USER_TOYIX_H
#define TOYIX_USER_TOYIX_H

#include "toyix_syscall.h"

typedef unsigned int toyix_size_t;

toyix_size_t toyix_strlen(const char *text);

void toyix_putchar(char ch);
void toyix_write_str(const char *text);
void toyix_puts(const char *text);

void toyix_write_uint(toyix_u32 value);
void toyix_write_int(toyix_i32 value);

int toyix_streq(const char *a, const char *b);

#endif
