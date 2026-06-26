// kernel/process.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "drivers/input/keyboard.h"
#include "kernel/address_space.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/process.h"
#include "kernel/string.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"

#define PROCESS_MAGIC 0x50524F43u

#define USER_PROCESS_CODE_VA   0x40100000u
#define USER_PROCESS_STACK_VA  0x40101000u
#define USER_PROCESS_STACK_TOP 0x40102000u
#define USER_PROCESS_PROMPT_VA 0x401000A0u
#define USER_PROCESS_PREFIX_VA 0x401000A8u
#define USER_PROCESS_NEWLINE_VA 0x401000B0u
#define USER_PROCESS_INPUT_VA  0x401000C0u

#define USER_PROCESS_INPUT_MAX 32u

static uint32_t next_pid;
static volatile uint32_t last_exit_code;
static volatile int last_exit_seen;

static void user_process_thread_entry(void *arg);

void process_init_system(void) {
    next_pid = 1;
    last_exit_code = 0xFFFFFFFFu;
    last_exit_seen = 0;

    console_writeln("Process: process table initialized");
}

static void map_user_page(address_space_t *space, uintptr_t virtual_addr) {
    uintptr_t physical = pmm_alloc_page();

    if (physical == PMM_INVALID_PAGE) {
        kernel_panic("process could not allocate user page");
    }

    int rc = address_space_map_page(
        space,
        virtual_addr,
        physical,
        ADDRESS_SPACE_FLAG_WRITABLE | ADDRESS_SPACE_FLAG_USER
    );

    if (rc != 0) {
        kernel_panic("process could not map user page");
    }
}

process_t *process_create_user(
    const char *name,
    const uint8_t *program,
    uint32_t program_size
) {
    if (program == 0 || program_size == 0) {
        kernel_panic("process_create_user received empty program");
    }

    if (program_size > PMM_PAGE_SIZE) {
        kernel_panic("process_create_user program too large for one page");
    }

    process_t *process = (process_t *)kcalloc(1, sizeof(process_t));

    if (process == 0) {
        kernel_panic("process_create_user could not allocate process object");
    }

    process->magic = PROCESS_MAGIC;
    process->pid = next_pid++;
    process->name = name != 0 ? name : "unnamed";
    process->state = PROCESS_NEW;

    process->exit_code = 0xFFFFFFFFu;
    process->exited = 0;

    process->user_code_base = USER_PROCESS_CODE_VA;
    process->user_stack_base = USER_PROCESS_STACK_VA;
    process->user_stack_top = USER_PROCESS_STACK_TOP;

    thread_t *thread = thread_create_suspended(
        process->name,
        user_process_thread_entry,
        process
    );

    process->address_space = address_space_create();

    map_user_page(process->address_space, process->user_code_base);
    map_user_page(process->address_space, process->user_stack_base);

    irq_flags_t flags = irq_save();
    address_space_t *old_space = address_space_current();

    address_space_switch(process->address_space);

    memset((void *)process->user_code_base, 0x90, PMM_PAGE_SIZE);
    memcpy((void *)process->user_code_base, program, program_size);
    memset((void *)process->user_stack_base, 0, PMM_PAGE_SIZE);

    address_space_switch(old_space);
    irq_restore(flags);

    thread_set_process(thread, process);

    process->main_thread = thread;
    process->state = PROCESS_RUNNING;

    thread_start(thread);

    console_write("Process: created pid=");
    console_write_u32_dec(process->pid);
    console_write(" name=");
    console_writeln(process->name);

    return process;
}

process_t *process_current(void) {
    thread_t *thread = thread_current();

    if (thread == 0) {
        return 0;
    }

    return (process_t *)thread_get_process(thread);
}

void process_exit_current(uint32_t exit_code) {
    process_t *process = process_current();

    if (process == 0) {
        console_write("Syscall: kernel thread exit code ");
        console_write_u32_dec(exit_code);
        console_putc('\n');

        last_exit_code = exit_code;
        last_exit_seen = 1;
        return;
    }

    process->exit_code = exit_code;
    process->exited = 1;
    process->state = PROCESS_EXITED;

    last_exit_code = exit_code;
    last_exit_seen = 1;

    console_write("Syscall: process ");
    console_write(process->name);
    console_write(" pid=");
    console_write_u32_dec(process->pid);
    console_write(" exited code ");
    console_write_u32_dec(exit_code);
    console_putc('\n');
}

uint32_t process_last_exit_code(void) {
    return (uint32_t)last_exit_code;
}

int process_last_exit_seen(void) {
    return last_exit_seen;
}

static void user_process_thread_entry(void *arg) {
    process_t *process = (process_t *)arg;

    if (process == 0 || process->magic != PROCESS_MAGIC) {
        kernel_panic("user process thread received invalid process");
    }

    usermode_enter_current_process();

    kernel_panic("user process returned from user mode");
}

static void emit_u8(uint8_t *program, uint32_t *offset, uint8_t value) {
    program[*offset] = value;
    (*offset)++;
}

static void emit_u32(uint8_t *program, uint32_t *offset, uint32_t value) {
    program[*offset + 0u] = (uint8_t)(value & 0xFFu);
    program[*offset + 1u] = (uint8_t)((value >> 8) & 0xFFu);
    program[*offset + 2u] = (uint8_t)((value >> 16) & 0xFFu);
    program[*offset + 3u] = (uint8_t)((value >> 24) & 0xFFu);
    *offset += 4u;
}

static void emit_mov_eax_imm32(
    uint8_t *program,
    uint32_t *offset,
    uint32_t value
) {
    emit_u8(program, offset, 0xB8u);
    emit_u32(program, offset, value);
}

static void emit_mov_ebx_imm32(
    uint8_t *program,
    uint32_t *offset,
    uint32_t value
) {
    emit_u8(program, offset, 0xBBu);
    emit_u32(program, offset, value);
}

static void emit_mov_ecx_imm32(
    uint8_t *program,
    uint32_t *offset,
    uint32_t value
) {
    emit_u8(program, offset, 0xB9u);
    emit_u32(program, offset, value);
}

static void emit_mov_edx_imm32(
    uint8_t *program,
    uint32_t *offset,
    uint32_t value
) {
    emit_u8(program, offset, 0xBAu);
    emit_u32(program, offset, value);
}

static void emit_int80(uint8_t *program, uint32_t *offset) {
    emit_u8(program, offset, 0xCDu);
    emit_u8(program, offset, 0x80u);
}

static void build_stdio_demo_program(uint8_t *program, uint32_t program_size) {
    memset(program, 0x90, program_size);

    uint32_t offset = 0;

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_PROMPT_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_READ);
    emit_mov_ebx_imm32(program, &offset, FD_STDIN);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_INPUT_VA);
    emit_mov_edx_imm32(program, &offset, USER_PROCESS_INPUT_MAX);
    emit_int80(program, &offset);

    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xC6u);

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_PREFIX_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_INPUT_VA);

    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xF2u);

    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, USER_PROCESS_NEWLINE_VA);
    emit_mov_edx_imm32(program, &offset, 1);
    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_SLEEP);
    emit_mov_ebx_imm32(program, &offset, 3);
    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_EXIT);
    emit_mov_ebx_imm32(program, &offset, 9);
    emit_int80(program, &offset);

    emit_u8(program, &offset, 0xEBu);
    emit_u8(program, &offset, 0xFEu);

    if (offset >= 0xA0u) {
        kernel_panic("stdio demo program overlapped data area");
    }

    const char prompt[] = "user> ";
    const char prefix[] = "echo: ";
    const char newline[] = "\n";

    memcpy(
        &program[USER_PROCESS_PROMPT_VA - USER_PROCESS_CODE_VA],
        prompt,
        sizeof(prompt) - 1u
    );

    memcpy(
        &program[USER_PROCESS_PREFIX_VA - USER_PROCESS_CODE_VA],
        prefix,
        sizeof(prefix) - 1u
    );

    memcpy(
        &program[USER_PROCESS_NEWLINE_VA - USER_PROCESS_CODE_VA],
        newline,
        1u
    );
}

void process_test_once(void) {
    console_writeln("Process test: starting isolated address-space syscall test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    static uint8_t program[256];

    build_stdio_demo_program(program, sizeof(program));

    process_create_user(
        "stdio-demo",
        program,
        sizeof(program)
    );

    thread_sleep_ticks(2);

    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('y');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('\n');

    while (!last_exit_seen) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (last_exit_code != 9) {
        kernel_panic("process address-space syscall test received wrong exit code");
    }

    console_writeln("Process test: isolated address-space syscall sanity check passed");
}
