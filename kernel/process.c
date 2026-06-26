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
#include "kernel/toyexe.h"
#include "kernel/usermode.h"

#define PROCESS_MAGIC 0x50524F43u

#define DEMO_IMAGE_SIZE 256u
#define DEMO_BSS_SIZE 64u

#define DEMO_PROMPT_VA (TOYEXE_USER_BASE + 0xA0u)
#define DEMO_PREFIX_VA (TOYEXE_USER_BASE + 0xA8u)
#define DEMO_NEWLINE_VA (TOYEXE_USER_BASE + 0xB0u)
#define DEMO_INPUT_VA (TOYEXE_USER_BASE + DEMO_IMAGE_SIZE)

#define DEMO_INPUT_MAX 32u

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

process_t *process_create_empty(const char *name) {
    process_t *process = (process_t *)kcalloc(1, sizeof(process_t));

    if (process == 0) {
        kernel_panic("process_create_empty could not allocate process object");
    }

    process->magic = PROCESS_MAGIC;
    process->pid = next_pid++;
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

    return process;
}

static uintptr_t page_align_down(uintptr_t value) {
    return value & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
}

static uintptr_t page_align_up(uintptr_t value) {
    return (value + PMM_PAGE_SIZE - 1u) & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
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
}

void process_start_user(process_t *process) {
    validate_process(process);

    if (process->user_entry == 0 ||
        process->user_stack_base == 0 ||
        process->user_stack_top == 0) {
        kernel_panic("process_start_user missing entry or stack");
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

    if (!process->exited) {
        kernel_panic("process_destroy called on running process");
    }

    uint32_t pid = process->pid;
    const char *name = process->name;

    if (process->address_space != 0) {
        address_space_destroy(process->address_space);
        process->address_space = 0;
    }

    process->main_thread = 0;
    process->state = PROCESS_DESTROYED;
    process->magic = 0;

    kfree(process);

    console_write("Process: destroyed pid=");
    console_write_u32_dec(pid);
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

static void build_stdio_demo_payload(uint8_t *program, uint32_t program_size) {
    memset(program, 0x90, program_size);

    uint32_t offset = 0;

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_PROMPT_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_READ);
    emit_mov_ebx_imm32(program, &offset, FD_STDIN);
    emit_mov_ecx_imm32(program, &offset, DEMO_INPUT_VA);
    emit_mov_edx_imm32(program, &offset, DEMO_INPUT_MAX);
    emit_int80(program, &offset);

    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xC6u);

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_PREFIX_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_INPUT_VA);

    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xF2u);

    emit_int80(program, &offset);

    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_NEWLINE_VA);
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
        &program[DEMO_PROMPT_VA - TOYEXE_USER_BASE],
        prompt,
        sizeof(prompt) - 1u
    );

    memcpy(
        &program[DEMO_PREFIX_VA - TOYEXE_USER_BASE],
        prefix,
        sizeof(prefix) - 1u
    );

    memcpy(
        &program[DEMO_NEWLINE_VA - TOYEXE_USER_BASE],
        newline,
        1u
    );
}

static uint32_t build_stdio_demo_toyexe(
    uint8_t *image,
    uint32_t image_capacity
) {
    uint32_t total_size = sizeof(toyexe_header_t) + DEMO_IMAGE_SIZE;

    if (image_capacity < total_size) {
        kernel_panic("TOYEXE demo image buffer too small");
    }

    memset(image, 0, image_capacity);

    toyexe_header_t *header = (toyexe_header_t *)image;

    header->magic = TOYEXE_MAGIC;
    header->version = TOYEXE_VERSION;
    header->header_size = sizeof(toyexe_header_t);
    header->entry_offset = 0;
    header->image_offset = sizeof(toyexe_header_t);
    header->image_size = DEMO_IMAGE_SIZE;
    header->bss_size = DEMO_BSS_SIZE;
    header->stack_size = TOYEXE_DEFAULT_STACK_SIZE;

    build_stdio_demo_payload(
        image + header->image_offset,
        DEMO_IMAGE_SIZE
    );

    return total_size;
}

void process_test_once(void) {
    console_writeln("Process test: starting TOYEXE lifecycle cleanup test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    static uint8_t toyexe_image[sizeof(toyexe_header_t) + DEMO_IMAGE_SIZE];

    uint32_t toyexe_size = build_stdio_demo_toyexe(
        toyexe_image,
        sizeof(toyexe_image)
    );

    process_t *process = toyexe_create_process(
        "toyexe-demo",
        toyexe_image,
        toyexe_size
    );

    thread_sleep_ticks(2);

    keyboard_debug_inject_char('t');
    keyboard_debug_inject_char('o');
    keyboard_debug_inject_char('y');
    keyboard_debug_inject_char('i');
    keyboard_debug_inject_char('x');
    keyboard_debug_inject_char('\n');

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("TOYEXE process test received wrong exit code");
    }

    process_destroy(process);

    if (last_exit_code != 9 || !last_exit_seen) {
        kernel_panic("TOYEXE process test missing exit diagnostics");
    }

    console_writeln("Process test: TOYEXE lifecycle cleanup sanity check passed");
}
