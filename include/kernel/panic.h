// include/kernel/panic.h
#ifndef TOYIX_KERNEL_PANIC_H
#define TOYIX_KERNEL_PANIC_H

void kernel_halt(void) __attribute__((noreturn));
void kernel_panic(const char *message) __attribute__((noreturn));

#endif
