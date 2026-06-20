// kernel/panic.c
#include "kernel/console.h"
#include "kernel/panic.h"


void kernel_halt(void) {
    __asm__ volatile ("cli");  // clear interrupt flag
    
    for (;;) {
        __asm__ volatile ("hlt"); // halt cpu   
    }   
}


void kernel_panic(const char *message) {
    console_writeln("");
    console_writeln("*** KERNEL PANIC ***");
    
    if (message != 0) {
        console_write("Reason: ");
        console_writeln(message);
    }
    
    console_writeln("System halted.");
    kernel_halt();
}
