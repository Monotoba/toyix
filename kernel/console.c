// kernel/console.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"

#define MAX_CONSOLE_DRIVERS 4

static const console_driver_t *drivers[MAX_CONSOLE_DRIVERS];
static size_t driver_count = 0;

void console_register(const console_driver_t * driver) {
    if (driver == NULL) {
        return;
    }
    
    if (driver_count >= MAX_CONSOLE_DRIVERS) {
        return;
    }
    
    drivers[driver_count++] = driver;
}


void console_init_all(void) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->init != NULL) {
            drivers[i]->init();
        }
    }
}


void console_putc(char c) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->putc != NULL) {
            drivers[i]->putc(c);
        }
    }

}


void console_write(const char *text) {
    if (text == NULL) {
        return;
    }
    
    while (*text != '\0') {
        console_putc(*text++);
    }
}


void console_writeln(const char *text) {
    console_write(text);
    console_putc('\n');
}



void console_write_hex32(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    
    console_write("0x");
    
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        console_putc(digits[nibble]);
    }
}



