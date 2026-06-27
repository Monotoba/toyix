// include/kernel/syscall.h
#ifndef TOYIX_KERNEL_SYSCALL_H
#define TOYIX_KERNEL_SYSCALL_H

#include "arch/x86/interrupts.h"

#define FD_STDIN  0u
#define FD_STDOUT 1u
#define FD_STDERR 2u

#define SYS_PUTC  1u
#define SYS_EXIT  2u
#define SYS_WRITE 3u
#define SYS_SLEEP 4u
#define SYS_READ  5u
#define SYS_EXEC  6u
#define SYS_WAITPID 7u
#define SYS_GETPID   8u
#define SYS_GETPPID  9u
#define SYS_PROCINFO 10u
#define SYS_KILL     11u
#define SYS_OPEN     12u
#define SYS_CLOSE    13u

typedef struct syscall_procinfo {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t exit_code;
    uint32_t exited;
    uint32_t kill_requested;
} syscall_procinfo_t;

void syscall_handler(interrupt_frame_t *frame);

#endif
