// kernel/process.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/irq_state.h"
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
#define PROCESS_SNAPSHOT_MAX 32u
#define PROCESS_MAX_ARGC 16
#define PROCESS_MAX_ARG_STRING 128

static uint32_t next_pid;
static volatile uint32_t last_exit_code;
static volatile int last_exit_seen;
static process_t *process_head;
static process_t *process_tail;
static uint32_t process_count;

static void user_process_thread_entry(void *arg);

void process_init_system(void) {
    next_pid = 1;
    last_exit_code = 0xFFFFFFFFu;
    last_exit_seen = 0;
    process_head = 0;
    process_tail = 0;
    process_count = 0;

    console_writeln("Process: process table initialized");
}

static void validate_process(process_t *process) {
    if (process == 0) {
        kernel_panic("process: null process");
    }

    if (process->magic != PROCESS_MAGIC) {
        kernel_panic("process: magic mismatch");
    }
}

static void validate_live_process(process_t *process) {
    validate_process(process);

    if (process->state == PROCESS_DESTROYED) {
        kernel_panic("process: operation on destroyed process");
    }
}

const char *process_state_name(process_state_t state) {
    switch (state) {
        case PROCESS_NEW:
            return "new";
        case PROCESS_RUNNING:
            return "running";
        case PROCESS_ZOMBIE:
            return "zombie";
        case PROCESS_DESTROYED:
            return "destroyed";
        default:
            return "unknown";
    }
}

static void process_table_insert(process_t *process) {
    validate_process(process);

    process->next = 0;
    process->prev = process_tail;

    if (process_tail != 0) {
        process_tail->next = process;
    } else {
        process_head = process;
    }

    process_tail = process;
    process_count++;
}

static void process_table_remove(process_t *process) {
    validate_process(process);

    if (process->prev != 0) {
        process->prev->next = process->next;
    } else {
        process_head = process->next;
    }

    if (process->next != 0) {
        process->next->prev = process->prev;
    } else {
        process_tail = process->prev;
    }

    process->next = 0;
    process->prev = 0;

    if (process_count > 0) {
        process_count--;
    }
}

process_t *process_create_empty(const char *name) {
    process_t *process = (process_t *)kcalloc(1, sizeof(process_t));

    if (process == 0) {
        kernel_panic("process_create_empty could not allocate process object");
    }

    process->magic = PROCESS_MAGIC;
    process->pid = next_pid++;
    process->parent_pid = 0;
    process->name = name != 0 ? name : "unnamed";
    process->state = PROCESS_NEW;
    process->address_space = address_space_create();
    process->main_thread = 0;
    process->exit_code = 0xFFFFFFFFu;
    process->exited = 0;
    process->user_code_base = 0;
    process->user_entry = 0;
    process->user_stack_base = 0;
    process->user_stack_top = 0;
    process->user_initial_esp = 0;
    process->next = 0;
    process->prev = 0;

    irq_flags_t flags = irq_save();
    process_table_insert(process);
    irq_restore(flags);

    return process;
}

void process_set_parent(process_t *process, uint32_t parent_pid) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    process->parent_pid = parent_pid;
    irq_restore(flags);
}

uint32_t process_parent_pid(process_t *process) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    uint32_t parent_pid = process->parent_pid;
    irq_restore(flags);

    return parent_pid;
}

int process_is_child_of(process_t *process, uint32_t parent_pid) {
    validate_live_process(process);

    irq_flags_t flags = irq_save();
    int result = process->parent_pid == parent_pid;
    irq_restore(flags);

    return result;
}

static uintptr_t page_align_down(uintptr_t value) {
    return value & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static uintptr_t page_align_up(uintptr_t value) {
    return (value + PMM_PAGE_SIZE - 1u) & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static uintptr_t align_down_4(uintptr_t value) {
    return value & ~(uintptr_t)0x3u;
}

static int map_user_page(process_t *process, uintptr_t virtual_addr) {
    validate_process(process);

    uintptr_t physical = pmm_alloc_page();

    if (physical == PMM_INVALID_PAGE) {
        return -1;
    }

    int rc = address_space_map_page(
        process->address_space,
        virtual_addr,
        physical,
        ADDRESS_SPACE_FLAG_WRITABLE | ADDRESS_SPACE_FLAG_USER
    );

    if (rc != 0) {
        pmm_free_page(physical);
        return -1;
    }

    return 0;
}

int process_map_user_region(
    process_t *process,
    uintptr_t virtual_addr,
    uint32_t size_bytes
) {
    validate_process(process);

    if (size_bytes == 0) {
        return 0;
    }

    uintptr_t start = page_align_down(virtual_addr);
    uintptr_t end_raw = virtual_addr + (uintptr_t)size_bytes;

    if (end_raw < virtual_addr) {
        return -1;
    }

    uintptr_t end = page_align_up(end_raw);

    for (uintptr_t addr = start; addr < end; addr += PMM_PAGE_SIZE) {
        if (map_user_page(process, addr) != 0) {
            return -1;
        }
    }

    return 0;
}

static void process_with_address_space(
    process_t *process,
    void (*operation)(void *context),
    void *context
) {
    validate_process(process);

    irq_flags_t flags = irq_save();
    address_space_t *old_space = address_space_current();

    address_space_switch(process->address_space);
    operation(context);
    address_space_switch(old_space);

    irq_restore(flags);
}

typedef struct copy_context {
    uintptr_t user_dest;
    const void *kernel_src;
    uint32_t size;
} copy_context_t;

static void copy_operation(void *context) {
    copy_context_t *copy = (copy_context_t *)context;
    memcpy((void *)copy->user_dest, copy->kernel_src, copy->size);
}

int process_copy_to_user_init(
    process_t *process,
    uintptr_t user_dest,
    const void *kernel_src,
    uint32_t size
) {
    validate_process(process);

    if (size == 0) {
        return 0;
    }

    if (kernel_src == 0) {
        return -1;
    }

    copy_context_t context;
    context.user_dest = user_dest;
    context.kernel_src = kernel_src;
    context.size = size;

    process_with_address_space(process, copy_operation, &context);
    return 0;
}

typedef struct zero_context {
    uintptr_t user_dest;
    uint32_t size;
} zero_context_t;

static void zero_operation(void *context) {
    zero_context_t *zero = (zero_context_t *)context;
    memset((void *)zero->user_dest, 0, zero->size);
}

int process_zero_user_init(
    process_t *process,
    uintptr_t user_dest,
    uint32_t size
) {
    validate_process(process);

    if (size == 0) {
        return 0;
    }

    zero_context_t context;
    context.user_dest = user_dest;
    context.size = size;

    process_with_address_space(process, zero_operation, &context);
    return 0;
}

void process_set_user_entry(process_t *process, uintptr_t entry) {
    validate_process(process);
    process->user_entry = entry;
}

void process_set_user_stack(
    process_t *process,
    uintptr_t stack_base,
    uintptr_t stack_top
) {
    validate_process(process);
    process->user_stack_base = stack_base;
    process->user_stack_top = stack_top;
    process->user_initial_esp = stack_top;
}

typedef struct argv_copy_context {
    uintptr_t dest;
    const void *src;
    uint32_t size;
} argv_copy_context_t;

static void argv_copy_operation(void *context) {
    argv_copy_context_t *copy = (argv_copy_context_t *)context;
    memcpy((void *)copy->dest, copy->src, copy->size);
}

static int process_copy_stack_bytes(
    process_t *process,
    uintptr_t dest,
    const void *src,
    uint32_t size
) {
    if (size == 0) {
        return 0;
    }

    argv_copy_context_t context;
    context.dest = dest;
    context.src = src;
    context.size = size;

    process_with_address_space(process, argv_copy_operation, &context);
    return 0;
}

int process_setup_arguments(
    process_t *process,
    int argc,
    const char **argv
) {
    validate_live_process(process);

    if (argc < 0 || argc > PROCESS_MAX_ARGC) {
        return -1;
    }

    if (argc > 0 && argv == 0) {
        return -1;
    }

    if (process->user_stack_base == 0 || process->user_stack_top == 0) {
        return -1;
    }

    uintptr_t sp = process->user_stack_top;
    uintptr_t arg_ptrs[PROCESS_MAX_ARGC];

    for (int i = argc - 1; i >= 0; --i) {
        const char *arg = argv[i];

        if (arg == 0) {
            return -1;
        }

        uint32_t len = 0;
        while (arg[len] != '\0') {
            len++;
            if (len >= PROCESS_MAX_ARG_STRING) {
                return -1;
            }
        }

        uint32_t bytes = len + 1u;
        if (sp < process->user_stack_base + bytes) {
            return -1;
        }

        sp -= bytes;
        if (process_copy_stack_bytes(process, sp, arg, bytes) != 0) {
            return -1;
        }

        arg_ptrs[i] = sp;
    }

    sp = align_down_4(sp);

    uint32_t pointer_words = 1u + (uint32_t)argc + 1u + 1u;
    uint32_t table_bytes = pointer_words * sizeof(uint32_t);

    if (sp < process->user_stack_base + table_bytes) {
        return -1;
    }

    sp -= table_bytes;

    uint32_t stack_table[1u + PROCESS_MAX_ARGC + 1u + 1u];
    uint32_t index = 0;

    stack_table[index++] = (uint32_t)argc;
    for (int i = 0; i < argc; ++i) {
        stack_table[index++] = (uint32_t)arg_ptrs[i];
    }
    stack_table[index++] = 0;
    stack_table[index++] = 0;

    if (process_copy_stack_bytes(process, sp, stack_table, table_bytes) != 0) {
        return -1;
    }

    process->user_initial_esp = sp;

    console_write("Process: initial stack argc=");
    console_write_u32_dec((uint32_t)argc);
    console_write(" esp=");
    console_write_hex32((uint32_t)sp);
    console_putc('\n');

    return 0;
}

void process_start_user(process_t *process) {
    validate_process(process);

    if (process->user_entry == 0 ||
        process->user_stack_base == 0 ||
        process->user_stack_top == 0 ||
        process->user_initial_esp == 0) {
        kernel_panic("process_start_user missing entry or initial stack");
    }

    thread_t *thread = thread_create_suspended(
        process->name,
        user_process_thread_entry,
        process
    );

    thread_set_process(thread, process);

    process->main_thread = thread;
    process->state = PROCESS_RUNNING;

    thread_start(thread);

    console_write("Process: created pid=");
    console_write_u32_dec(process->pid);
    console_write(" name=");
    console_writeln(process->name);
}

process_t *process_current(void) {
    thread_t *thread = thread_current();

    if (thread == 0) {
        return 0;
    }

    return (process_t *)thread_get_process(thread);
}

process_t *process_find(uint32_t pid) {
    irq_flags_t flags = irq_save();
    process_t *cur = process_head;

    while (cur != 0) {
        if (cur->pid == pid) {
            irq_restore(flags);
            return cur;
        }

        cur = cur->next;
    }

    irq_restore(flags);
    return 0;
}

typedef struct process_snapshot {
    uint32_t pid;
    uint32_t parent_pid;
    process_state_t state;
    uint32_t exit_code;
    int exited;
    const char *name;
} process_snapshot_t;

static void print_padded_u32(uint32_t value, uint32_t width) {
    console_write_u32_dec(value);

    uint32_t digits = 1;
    uint32_t tmp = value;

    while (tmp >= 10u) {
        tmp /= 10u;
        digits++;
    }

    while (digits < width) {
        console_putc(' ');
        digits++;
    }
}

void process_list(void) {
    process_snapshot_t snapshots[PROCESS_SNAPSHOT_MAX];
    uint32_t count = 0;

    irq_flags_t flags = irq_save();
    process_t *cur = process_head;

    while (cur != 0 && count < PROCESS_SNAPSHOT_MAX) {
        snapshots[count].pid = cur->pid;
        snapshots[count].parent_pid = cur->parent_pid;
        snapshots[count].state = cur->state;
        snapshots[count].exit_code = cur->exit_code;
        snapshots[count].exited = cur->exited;
        snapshots[count].name = cur->name;
        count++;
        cur = cur->next;
    }

    uint32_t total = process_count;
    irq_restore(flags);

    console_writeln("PID  PPID STATE     EXIT  NAME");

    for (uint32_t i = 0; i < count; ++i) {
        print_padded_u32(snapshots[i].pid, 5);
        print_padded_u32(snapshots[i].parent_pid, 5);

        const char *state = process_state_name(snapshots[i].state);
        console_write(state);

        uint32_t state_len = (uint32_t)kstrlen(state);
        while (state_len < 10) {
            console_putc(' ');
            state_len++;
        }

        if (snapshots[i].exited) {
            console_write_u32_dec(snapshots[i].exit_code);
        } else {
            console_putc('-');
        }

        console_write("     ");
        console_writeln(snapshots[i].name);
    }

    if (total > count) {
        console_write("ps: truncated process list at ");
        console_write_u32_dec(PROCESS_SNAPSHOT_MAX);
        console_writeln(" entries");
    }
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
    process->state = PROCESS_ZOMBIE;

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

uint32_t process_wait(process_t *process) {
    validate_live_process(process);

    if (process_current() == process) {
        kernel_panic("process attempted to wait on itself");
    }

    while (!process->exited) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    return process->exit_code;
}

void process_destroy(process_t *process) {
    validate_live_process(process);

    if (!process->exited && process->state != PROCESS_ZOMBIE) {
        kernel_panic("process_destroy called on running process");
    }

    uint32_t pid = process->pid;
    uint32_t parent_pid = process->parent_pid;
    const char *name = process->name;

    irq_flags_t flags = irq_save();
    process_t *cur = process_head;

    while (cur != 0) {
        if (cur->parent_pid == pid) {
            cur->parent_pid = 0;
        }

        cur = cur->next;
    }

    process_table_remove(process);
    irq_restore(flags);

    if (process->address_space != 0) {
        address_space_destroy(process->address_space);
        process->address_space = 0;
    }

    process->state = PROCESS_DESTROYED;
    process->magic = 0;

    kfree(process);

    console_write("Process: destroyed pid=");
    console_write_u32_dec(pid);
    console_write(" ppid=");
    console_write_u32_dec(parent_pid);
    console_write(" name=");
    console_writeln(name);
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
