// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/idle.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;


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
    pic_init();
    
    
    pit_init(100);
    keyboard_init();
    
    console_writeln("Interrupt hardware: configured");
    
#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

	interrupts_enable();
	
	console_writeln("Interrupts: enabled");
	
	pit_wait_ticks(3);
	console_writeln("Timer: observed 3 ticks");
    
    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: physical memory map and page allocator.");
    
    kernel_idle();  /* kernel_idle waits on interrupts but does not */
    				/*clear them like kernel halt() */
 }





