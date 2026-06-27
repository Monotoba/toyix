// kernel/syscall.c
#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/process.h"
#include "kernel/program.h"
#include "kernel/syscall.h"
#include "kernel/terminal.h"
#include "kernel/thread.h"
#include "kernel/usercopy.h"
#include "kernel/vfs.h"

#define SYSCALL_RW_MAX 256u
#define SYSCALL_EXEC_MAX_ARGS    8u
#define SYSCALL_EXEC_MAX_NAME    32u
#define SYSCALL_EXEC_MAX_ARG_LEN 64u
#define SYSCALL_PATH_MAX         64u

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

    if (length > SYSCALL_RW_MAX) {
        length = SYSCALL_RW_MAX;
    }

    if (length == 0) {
        frame->eax = 0;
        return;
    }

    if (fd == FD_STDIN) {
        char buffer[SYSCALL_RW_MAX + 1u];
        size_t got = 0;

        interrupts_enable();
        got = terminal_readline(buffer, (size_t)length + 1u);

        if (copy_to_user(user_buf, buffer, got) != USERCOPY_OK) {
            frame->eax = 0xFFFFFFFFu;
            return;
        }

        frame->eax = (uint32_t)got;
        return;
    }

    process_t *current = process_current();
    vfs_file_t *file = 0;
    char buffer[SYSCALL_RW_MAX];
    uint32_t got = 0;

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    file = process_fd_get(current, fd);
    if (file == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (vfs_read(file, buffer, length, &got) != VFS_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (copy_to_user(user_buf, buffer, got) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = (uint32_t)got;
}

static void syscall_open(interrupt_frame_t *frame) {
    uintptr_t user_path = (uintptr_t)frame->ebx;
    char path[SYSCALL_PATH_MAX];
    vfs_file_t *file = 0;
    process_t *current = process_current();
    int fd = -1;

    (void)frame->ecx;

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (syscall_copy_user_string(user_path, path, sizeof(path)) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (vfs_open(path, &file) != VFS_OK || file == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    fd = process_fd_install(current, file);
    if (fd < 0) {
        vfs_close(file);
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = (uint32_t)fd;
}

static void syscall_close(interrupt_frame_t *frame) {
    uint32_t fd = frame->ebx;
    process_t *current = process_current();

    if (current == 0 || fd < PROCESS_FIRST_FILE_FD) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (process_fd_close(current, fd) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = 0;
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

static void syscall_getpid(interrupt_frame_t *frame) {
    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0;
        return;
    }

    frame->eax = current->pid;
}

static void syscall_getppid(interrupt_frame_t *frame) {
    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0;
        return;
    }

    frame->eax = process_parent_pid(current);
}

static void syscall_procinfo(interrupt_frame_t *frame) {
    uint32_t pid = frame->ebx;
    uintptr_t user_info = (uintptr_t)frame->ecx;
    process_t *current = process_current();
    process_t *process = 0;
    syscall_procinfo_t info;

    if (current == 0 || user_info == 0 || pid == 0) {
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

    irq_flags_t flags = irq_save();
    info.pid = process->pid;
    info.parent_pid = process->parent_pid;
    info.state = (uint32_t)process->state;
    info.exit_code = process->exit_code;
    info.exited = (uint32_t)(process->exited != 0);
    info.kill_requested = (uint32_t)(process->kill_requested != 0);
    irq_restore(flags);

    if (copy_to_user(user_info, &info, sizeof(info)) != USERCOPY_OK) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = 0;
}

static void syscall_kill(interrupt_frame_t *frame) {
    uint32_t target_pid = frame->ebx;
    process_t *current = process_current();

    if (current == 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (target_pid == 0 || target_pid == current->pid) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    if (process_request_kill_child(current->pid, target_pid) != 0) {
        frame->eax = 0xFFFFFFFFu;
        return;
    }

    frame->eax = 0;
}

static int syscall_finish_or_kill(interrupt_frame_t *frame) {
    (void)frame;

    process_t *current = process_current();

    if (current == 0) {
        return 0;
    }

    if (!process_kill_requested(current)) {
        return 0;
    }

    process_exit_current(PROCESS_KILLED_EXIT_CODE);
    thread_exit();

    return 1;
}

void syscall_handler(interrupt_frame_t *frame) {
    if (frame == 0) {
        return;
    }

    uint32_t number = frame->eax;

    if (number != SYS_EXIT &&
        number != SYS_WAITPID &&
        number != SYS_KILL) {
        if (syscall_finish_or_kill(frame)) {
            return;
        }
    }

    switch (number) {
        case SYS_PUTC: {
            char ch = (char)(frame->ebx & 0xFFu);
            console_putc(ch);
            frame->eax = 0;
            syscall_finish_or_kill(frame);
            return;
        }

        case SYS_READ:
            syscall_read(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_WRITE:
            syscall_write(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_SLEEP: {
            uint32_t ticks = frame->ebx;
            interrupts_enable();
            thread_sleep_ticks(ticks);
            frame->eax = 0;
            syscall_finish_or_kill(frame);
            return;
        }

        case SYS_EXEC:
            syscall_exec(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_WAITPID:
            syscall_waitpid(frame);
            return;

        case SYS_GETPID:
            syscall_getpid(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_GETPPID:
            syscall_getppid(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_PROCINFO:
            syscall_procinfo(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_KILL:
            syscall_kill(frame);
            return;

        case SYS_OPEN:
            syscall_open(frame);
            syscall_finish_or_kill(frame);
            return;

        case SYS_CLOSE:
            syscall_close(frame);
            syscall_finish_or_kill(frame);
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
