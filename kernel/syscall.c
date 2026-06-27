// kernel/syscall.c
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/syscall.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"
#include "kernel/usercopy.h"

#define SYSCALL_RW_MAX 256u
#define SYSCALL_EXEC_MAX_ARGS    8u
#define SYSCALL_EXEC_MAX_NAME    32u
#define SYSCALL_EXEC_MAX_ARG_LEN 64u

static int syscall_copy_user_string(
    uintptr_t user_str,
    char *kernel_buffer,
    uint32_t kernel_buffer_size
) {
    if (user_str == 0 ||
        kernel_buffer == 0 ||
        kernel_buffer_size == 0) {
        return -1;
    }

    size_t length = 0;

    if (user_string_length(
            user_str,
            kernel_buffer_size,
            &length
        ) != USERCOPY_OK) {
        return -1;
    }

    if (length + 1u > kernel_buffer_size) {
        return -1;
    }

    if (copy_from_user(
            kernel_buffer,
            user_str,
            length + 1u
        ) != USERCOPY_OK) {
        return -1;
    }

    return 0;
}

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

static void syscall_exec(interrupt_frame_t *frame) {
    uintptr_t user_name = (uintptr_t)frame->ebx;
    uintptr_t user_argv = (uintptr_t)frame->ecx;
    uint32_t argc = frame->edx;
    char name[SYSCALL_EXEC_MAX_NAME];
    char arg_storage[SYSCALL_EXEC_MAX_ARGS][SYSCALL_EXEC_MAX_ARG_LEN];
    const char *kernel_argv[SYSCALL_EXEC_MAX_ARGS];
    process_t *child = 0;

    if (syscall_copy_user_string(
            user_name,
            name,
            sizeof(name)
        ) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (argc == 0 || user_argv == 0) {
        argc = 1;
        kernel_argv[0] = name;
    } else {
        if (argc > SYSCALL_EXEC_MAX_ARGS) {
            frame->eax = 0xFFFFFFFFu;
            return;
        }

        for (uint32_t i = 0; i < argc; ++i) {
            uintptr_t user_arg_ptr = 0;

            if (copy_from_user(
                    &user_arg_ptr,
                    user_argv + i * sizeof(uintptr_t),
                    sizeof(uintptr_t)
                ) != USERCOPY_OK) {
                frame->eax = 0xFFFFFFFFu;
                return;
            }

            if (syscall_copy_user_string(
                    user_arg_ptr,
                    arg_storage[i],
                    SYSCALL_EXEC_MAX_ARG_LEN
                ) != 0) {
                frame->eax = 0xFFFFFFFFu;
                return;
            }

            kernel_argv[i] = arg_storage[i];
        }
    }

    if (program_run_background(name, (int)argc, kernel_argv, &child) != 0 ||
        child == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = child->pid;
}

static void syscall_waitpid(interrupt_frame_t *frame) {
    uint32_t pid = frame->ebx;
    uintptr_t user_status = (uintptr_t)frame->ecx;
    process_t *current = process_current();
    process_t *process = 0;
    uint32_t status = 0;

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    process = process_find(pid);
    if (process == 0 || process == current) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (!process_is_child_of(process, current->pid)) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    interrupts_enable();
    status = process_wait(process);

    if (user_status != 0) {
        if (copy_to_user(
                user_status,
                &status,
                sizeof(status)
            ) != USERCOPY_OK) {
            frame->eax = 0xFFFFFFFFu;
            return;
        }
    }

    process_destroy(process);
    frame->eax = 0;
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

        case SYS_EXEC:
            syscall_exec(frame);
            return;

        case SYS_WAITPID:
            syscall_waitpid(frame);
            return;

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
