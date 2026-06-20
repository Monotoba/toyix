// drivers/console/serial.c
#include <stdint.h>
#include "kernel/console.h"
#include "arch/x86/io.h"

#define COM1 0x3F8

static int serial_transmit_ready(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);   // Disable interrupts
    outb(COM1 + 3, 0x80);   // Enable DLAB: divisor access
    outb(COM1 + 0, 0x03);   // Divisor low byte: 38400 baud
    outb(COM1 + 1, 0x00);   // Divisor high byte
    outb(COM1 + 3, 0x03);   // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);   // Enable FIFIO, clear it, 14-byte threshold
    outb(COM1 + 4, 0x0B);   // IRQsd enable, RTS/DSR set
}


static void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    
    for (uint32_t timeout = 0; timeout < 100000; ++timeout) {
        if (serial_transmit_ready()) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}


const console_driver_t serial_console_driver = {
    .name = "serial",
    .init = serial_init,
    .putc = serial_putc
};




