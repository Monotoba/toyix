// include/kernel/console.h
#ifndef TOYIX_KERNEL_CONSOLE_H
#define TOYIX_KERNEL_CONSOLE_H

#include <stdint.h>

typedef struct console_driver {
    const char *name;
    void  (*init)(void);
    void  (*putc)(char c);
} console_driver_t;

void console_register(const console_driver_t * driver);
void console_init_all(void);

void console_putc(char c);
void console_write(const char *text);
void console_writeln(const char *text);
void console_write_hex32(uint32_t value);
void console_write_u32_dec(uint32_t value);

#endif

