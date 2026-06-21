// kernel/idle.h
#ifndef TOYIX_KERNEL_IDLE_H
#define TOYIX_KERNEL_IDLE_H

/*
 * Enter the kernel's normal idle loop.
 *
 * Hardware interrupts must already be enabled. The processor sleeps between
 * interrupts but continues servicing timer, keyboard, and other IRQs.
 */
_Noreturn void kernel_idle(void);

#endif
