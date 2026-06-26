// kernel/syscall.c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/process.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usercopy.h"

#define SYSCALL_WRITE_MAX 256u

static void syscall_write(interrupt_frame_t *frame) {
    uintptr_t user_buf = (uintptr_t)frame->ebx;
    uint32_t length = frame->ecx;

    if (length > SYSCALL_WRITE_MAX) {
        length = SYSCALL_WRITE_MAX;
    }

    char buffer[SYSCALL_WRITE_MAX + 1u];

    if (copy_from_user(buffer, user_buf, length) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    buffer[length] = '\0';
    console_write(buffer);

    frame->eax = length;
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
