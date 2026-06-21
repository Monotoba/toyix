// kernel/idle.c
#include "arch/x86/interrupts.h"
#include "kernel/idle.h"

_Noreturn void kernel_idle(void) {
    for (;;) {
        interrupts_wait();
    }
}
