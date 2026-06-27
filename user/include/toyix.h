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
void toyix_write_hex(toyix_u32 value);

int toyix_streq(const char *a, const char *b);
int toyix_strcmp(const char *a, const char *b);
int toyix_isspace(char ch);
int toyix_atoi(const char *text, toyix_i32 *out_value);
toyix_i32 toyix_readline(char *buffer, toyix_u32 size);

typedef __builtin_va_list toyix_va_list;

#define toyix_va_start(ap, last) __builtin_va_start(ap, last)
#define toyix_va_end(ap) __builtin_va_end(ap)
#define toyix_va_arg(ap, type) __builtin_va_arg(ap, type)

void toyix_vprintf(const char *format, toyix_va_list ap);
void toyix_printf(const char *format, ...);

#endif
