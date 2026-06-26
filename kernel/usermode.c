// kernel/usermode.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"

extern void x86_enter_user_mode(uint32_t user_eip, uint32_t user_esp);

static uint32_t current_thread_kernel_stack_top(void) {
    thread_t *self = thread_current();

    if (self == 0 || self->stack_base == 0) {
        kernel_panic("user mode entry requires current thread stack");
    }

    return (uint32_t)((uintptr_t)self->stack_base + self->stack_size);
}

void usermode_enter_current_process(void) {
    process_t *process = process_current();

    if (process == 0) {
        kernel_panic("usermode entry without process");
    }

    if (process->user_entry == 0 || process->user_initial_esp == 0) {
        kernel_panic("usermode entry missing entry point or initial stack");
    }

    tss_set_kernel_stack(current_thread_kernel_stack_top());

    x86_enter_user_mode(
        (uint32_t)process->user_entry,
        (uint32_t)process->user_initial_esp
    );

    kernel_panic("x86_enter_user_mode returned unexpectedly");
}
