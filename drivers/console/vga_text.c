// drivers/console/vga_text.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14,
    VGA_WHITE = 15 
};


static volatile uint16_t * const vga_buffer = (volatile uint16_t *)0xB8000;

static size_t row;
static size_t column;
static uint8_t color;


static uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t)(fg | (bg << 4));
}


static uint16_t vga_entry(unsigned char ch, uint8_t entry_color) {
    return (uint16_t)ch | ((uint16_t) entry_color << 8);
}


static void vga_clear_row(size_t y) {
    for (size_t x = 0; x < VGA_WIDTH; ++x) {
        vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', color);
    }
}


static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; ++y) {
        for (size_t x = 0; x < VGA_WIDTH; ++x) {
            vga_buffer[(y - 1) * VGA_WIDTH + x] =
                vga_buffer[y * VGA_WIDTH + x];
        }
    }
    
    vga_clear_row(VGA_HEIGHT - 1);
    row = VGA_HEIGHT - 1;
}


static void vga_newline(void) {
    column = 0;
    ++row;
    
    if (row >= VGA_HEIGHT) {
        vga_scroll();
    }
}


static void vga_putc(char c) {
    if (c == '\n') {
        vga_newline();
        return;
    }
    
    vga_buffer[row * VGA_WIDTH + column] =
        vga_entry((unsigned char)c, color);
        
    ++column;
    
    if (column >= VGA_WIDTH) {
        vga_newline();
    }
}


static void vga_init(void) {
    row = 0;
    column = 0;
    color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    
    for (size_t y = 0; y < VGA_HEIGHT; ++y) {
        vga_clear_row(y);
    }
}


const console_driver_t vga_text_console_driver = {
    .name = "vga_text",
    .init = vga_init,
    .putc = vga_putc
};


