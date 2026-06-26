// include/kernel/syscall.h
#ifndef TOYIX_KERNEL_SYSCALL_H
#define TOYIX_KERNEL_SYSCALL_H

#include "arch/x86/interrupts.h"

#define SYS_PUTC  1u
#define SYS_EXIT  2u
#define SYS_WRITE 3u
#define SYS_SLEEP 4u

void syscall_handler(interrupt_frame_t *frame);

#endif
