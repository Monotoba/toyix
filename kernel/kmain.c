// kernel/kmain.c
#include <stdint.h>
#include "kernel/console.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}


void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    console_register(&serial_console_driver);
    console_register(&vga_text_console_driver);
    console_init_all();
    
    console_writeln("Toyix kernel alive!");
    
    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        console_write("Boot protocol: Multiboot OK");
    } else {
        console_write("Boot protocol: unexpected magic ");
        console_write_hex32(multiboot_magic);
        console_putc('\n');
    }
    
    console_write("Multiboot info at ");
    console_write_hex32(multiboot_info_addr);
    console_putc('\n');
    
    console_writeln("Console drivers: serial + VGA text");
    console_writeln("Next stop: GDT, IDT, memory map, heap.");
    
    halt_forever();
}






