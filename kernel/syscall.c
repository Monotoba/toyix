// kernel/syscall.c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"

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

        case SYS_EXIT: {
            uint32_t exit_code = frame->ebx;

            console_writeln("");
            console_write("Syscall: user thread requested exit code ");
            console_write_u32_dec(exit_code);
            console_putc('\n');

            usermode_notify_exit(exit_code);
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
