// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "kernel/console.h"
#include "kernel/panic.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;


// Removed code here

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    console_register(&serial_console_driver);
    console_register(&vga_text_console_driver);
    console_init_all();
    
    console_writeln("Toyix kernel alive!");
    
    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        console_writeln("Boot protocol: Multiboot OK");
    } else {
        console_write("Boot protocol: unexpected magic ");
        console_write_hex32(multiboot_magic);
        console_putc('\n');
    }
    
    console_write("Multiboot info at ");
    console_write_hex32(multiboot_info_addr);
    console_putc('\n');
    
    // Added GDT / IDT table init calls
    gdt_init();
    idt_init();
    
    console_writeln("Descriptor tables: ready");
    
#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif
    
    console_writeln("Kernel survived early CPU setup.");
    console_writeln("Next stop: PIC remap, time IRQ, keyboard IRQ.");
    
    kernel_halt();
}






