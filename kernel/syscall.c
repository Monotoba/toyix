// kernel/syscall.c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/process.h"
#include "kernel/syscall.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"
#include "kernel/usercopy.h"

#define SYSCALL_RW_MAX 256u

static void syscall_write(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    uintptr_t user_buf = (uintptr_t)frame->ecx;
    uint32_t length = frame->edx;

    if (fd != FD_STDOUT && fd != FD_STDERR) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    if (length == 0) {
        frame->eax = 0;
        return;
    }

    char buffer[SYSCALL_RW_MAX];

    if (copy_from_user(buffer, user_buf, length) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    console_write_n(buffer, length);

    frame->eax = length;
}

static void syscall_read(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    uintptr_t user_buf = (uintptr_t)frame->ecx;
    uint32_t length = frame->edx;

    if (fd != FD_STDIN) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    if (length == 0) {
        frame->eax = 0;
        return;
    }

    char buffer[SYSCALL_RW_MAX + 1u];

    interrupts_enable();
    size_t got = terminal_readline(buffer, (size_t)length + 1u);

    if (copy_to_user(user_buf, buffer, got) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = (uint32_t)got;
}

void syscall_handler(interrupt_frame_t *frame) {
    if (frame == 0) {
        return;
    }

    uint32_t number = frame->eax;

    switch (number) {
        case SYS_PUTC: {
            char ch = (char)(frame->ebx & 0xFFu);
            console_putc(ch);
            frame->eax = 0;
            return;
        }

        case SYS_READ:
            syscall_read(frame);
            return;

        case SYS_WRITE:
            syscall_write(frame);
            return;

        case SYS_SLEEP: {
            uint32_t ticks = frame->ebx;
            interrupts_enable();
            thread_sleep_ticks(ticks);
            frame->eax = 0;
            return;
        }

        case SYS_EXIT: {
            uint32_t exit_code = frame->ebx;

            process_exit_current(exit_code);
            thread_exit();
            return;
        }

        default:
            console_write("Syscall: unknown syscall ");
            console_write_u32_dec(number);
            console_putc('\n');

            frame->eax = 0xFFFFFFFFu;
            return;
    }
}
