// kernel/process.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/process.h"
#include "kernel/string.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"
#include "kernel/vmem.h"

#define PROCESS_MAGIC 0x50524F43u

#define USER_PROCESS_CODE_VA   0x40100000u
#define USER_PROCESS_STACK_VA  0x40101000u
#define USER_PROCESS_STACK_TOP 0x40102000u

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

static void map_user_page(uintptr_t virtual_addr) {
    uintptr_t physical = pmm_alloc_page();

    if (physical == PMM_INVALID_PAGE) {
        kernel_panic("process could not allocate user page");
    }

    int rc = vmem_map_page(
        virtual_addr,
        physical,
        VMEM_FLAG_WRITABLE | VMEM_FLAG_USER
    );

    if (rc != VMEM_OK) {
        kernel_panic("process could not map user page");
    }

    memset((void *)virtual_addr, 0, PMM_PAGE_SIZE);
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

    map_user_page(process->user_code_base);
    map_user_page(process->user_stack_base);

    memcpy((void *)process->user_code_base, program, program_size);

    thread_t *thread = thread_create(
        process->name,
        user_process_thread_entry,
        process
    );

    thread_set_process(thread, process);

    process->main_thread = thread;
    process->state = PROCESS_RUNNING;

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

static const uint8_t user_process_demo[] = {
    0xB8, SYS_WRITE, 0x00, 0x00, 0x00,
    0xBB, 0x40,      0x00, 0x10, 0x40,
    0xB9, 0x2Au,     0x00, 0x00, 0x00,
    0xCD, 0x80,

    0xB8, SYS_SLEEP, 0x00, 0x00, 0x00,
    0xBB, 0x03,      0x00, 0x00, 0x00,
    0xCD, 0x80,

    0xB8, SYS_EXIT,  0x00, 0x00, 0x00,
    0xBB, 0x07,      0x00, 0x00, 0x00,
    0xCD, 0x80,

    0xEB, 0xFE,

    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90,

    'U','s','e','r',' ','p','r','o','c','e','s','s',' ',
    's','a','y','s',' ','h','e','l','l','o',' ','t','h',
    'r','o','u','g','h',' ','S','Y','S','_','W','R','I','T','E','\n'
};

void process_test_once(void) {
    console_writeln("Process test: starting user process syscall test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    process_create_user(
        "user-demo",
        user_process_demo,
        sizeof(user_process_demo)
    );

    while (!last_exit_seen) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (last_exit_code != 7) {
        kernel_panic("process test received wrong exit code");
    }

    console_writeln("Process test: user process syscall/write/sleep/exit sanity check passed");
}
