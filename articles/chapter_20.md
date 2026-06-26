# Chapter 20 — A Tiny Executable Format and User Program Loader

In Chapter 19, we gave each process its own address space:

```text
process A
  CR3 -> page directory A
  0x40100000 -> physical page A1

process B
  CR3 -> page directory B
  0x40100000 -> physical page B1
```

That was a major architectural milestone.

But our user program is still not really “loaded.” We are still building a TOYEXE image inside `process.c` and handing it to the loader.

This chapter adds a small executable format before jumping to ELF.

We will call it:

```text
TOYEXE
```

The goal is not to invent a serious executable format. The goal is to separate these responsibilities:

```text
process management
  owns PID, address space, main thread, exit state

loader
  validates executable image
  maps user pages
  copies program bytes
  zeroes BSS
  sets entry point and stack
```

After this chapter, the kernel will be able to do this:

```text
TOYEXE image
  ↓
toyexe_create_process()
  ↓
process_create_empty()
  ↓
map user image pages
  ↓
map user stack pages
  ↓
copy image into process address space
  ↓
start user thread
```

The milestone output will become:

```text
TOYEXE: loaded image bytes=256 bss=64
Process: created pid=1 name=toyexe-demo
user> toyix
echo: toyix
Syscall: process toyexe-demo pid=1 exited code 9
Process test: TOYEXE load/read/write/sleep/exit sanity check passed
```

---

# 1. Why not jump directly to ELF?

ELF is the right long-term target, but it adds several concepts at once:

```text
ELF header validation
program headers
section headers
loadable segments
alignment rules
entry address
permissions
relocations, eventually
symbol tables, eventually
```

Before that, we want a tiny loader that proves the shape:

```text
validate executable header
map memory
copy initialized image
zero BSS
set entry point
start process
```

Once that works, replacing TOYEXE with ELF becomes a loader upgrade, not a process-system rewrite.

---

# 2. TOYEXE format

Our first format is deliberately simple.

A TOYEXE image is:

```text
toyexe_header_t
raw image bytes
```

At runtime, the loader maps the raw image bytes at:

```text
0x40100000
```

Then it zeroes BSS immediately after the copied image.

The user stack is mapped separately near:

```text
0x70000000
```

The header tells the loader:

```text
magic
version
header size
entry offset
image offset
image size
BSS size
stack size
```

For example:

```text
entry address = TOYEXE_USER_BASE + entry_offset
```

---

# 3. Patch overview

Add:

```text
include/kernel/
└── toyexe.h

kernel/
└── toyexe.c
```

Modify:

```text
include/kernel/process.h
kernel/process.c
kernel/usermode.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

The old process code directly built and launched a raw user byte array.

After this chapter:

```text
process.c
  builds test TOYEXE image

toyexe.c
  validates and loads TOYEXE

process.c
  waits for process exit and verifies result
```

---

# 4. Add `include/kernel/toyexe.h`

```c
// include/kernel/toyexe.h
#ifndef TOYIX_KERNEL_TOYEXE_H
#define TOYIX_KERNEL_TOYEXE_H

#include <stdint.h>
#include "kernel/process.h"

#define TOYEXE_MAGIC   0x45595854u  /* 'T''X''Y''E' little-endian-ish marker */
#define TOYEXE_VERSION 1u

#define TOYEXE_USER_BASE         0x40100000u
#define TOYEXE_DEFAULT_STACK_TOP 0x70000000u
#define TOYEXE_DEFAULT_STACK_SIZE 4096u

#define TOYEXE_OK 0
#define TOYEXE_ERR_INVALID -1
#define TOYEXE_ERR_UNSUPPORTED -2
#define TOYEXE_ERR_TOO_LARGE -3
#define TOYEXE_ERR_LOAD_FAILED -4

typedef struct toyexe_header {
    uint32_t magic;
    uint32_t version;

    uint32_t header_size;

    uint32_t entry_offset;

    uint32_t image_offset;
    uint32_t image_size;

    uint32_t bss_size;
    uint32_t stack_size;
} __attribute__((packed)) toyexe_header_t;

int toyexe_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
);

process_t *toyexe_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
);

#endif
```

## Why the magic value looks odd

The exact number is not important as long as it is stable.

This:

```c
#define TOYEXE_MAGIC 0x45595854u
```

is just a recognizable marker. In memory, on little-endian x86, it appears as bytes:

```text
54 58 59 45
```

which loosely corresponds to:

```text
T X Y E
```

You can change the marker later if you want something cleaner. The important thing is that the loader refuses images without the expected magic.

---

# 5. Update `include/kernel/process.h`

We need to expose a few process-building helpers to the loader.

Replace the header with this version.

```c
// include/kernel/process.h
#ifndef TOYIX_KERNEL_PROCESS_H
#define TOYIX_KERNEL_PROCESS_H

#include <stdint.h>
#include "kernel/address_space.h"

struct thread;

typedef enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_EXITED
} process_state_t;

typedef struct process {
    uint32_t magic;
    uint32_t pid;

    const char *name;
    process_state_t state;

    address_space_t *address_space;

    struct thread *main_thread;

    uint32_t exit_code;
    int exited;

    uintptr_t user_code_base;
    uintptr_t user_entry;

    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
} process_t;

void process_init_system(void);

process_t *process_create_empty(const char *name);

int process_map_user_region(
    process_t *process,
    uintptr_t virtual_addr,
    uint32_t size_bytes
);

int process_copy_to_user_init(
    process_t *process,
    uintptr_t user_dest,
    const void *kernel_src,
    uint32_t size
);

int process_zero_user_init(
    process_t *process,
    uintptr_t user_dest,
    uint32_t size
);

void process_set_user_entry(process_t *process, uintptr_t entry);
void process_set_user_stack(
    process_t *process,
    uintptr_t stack_base,
    uintptr_t stack_top
);

void process_start_user(process_t *process);

process_t *process_current(void);

void process_exit_current(uint32_t exit_code);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

void process_test_once(void);

#endif
```

## What changed?

Instead of one monolithic function:

```c
toyexe_create_process(name, image, size)
```

we now have smaller pieces:

```text
create empty process
map user region
copy image bytes
zero BSS
set entry
set stack
start process
```

That is the loader-friendly design.

---

# 6. Replace the process creation core in `kernel/process.c`

Keep the test helpers for now, but replace the process creation machinery with this structure.

At the top, make sure the includes contain:

```c
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
```

Then use these constants and globals:

```c
#define PROCESS_MAGIC 0x50524F43u

static uint32_t next_pid;
static volatile uint32_t last_exit_code;
static volatile int last_exit_seen;

static void user_process_thread_entry(void *arg);
```

Now add the new implementation.

```c
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
    return (value + PMM_PAGE_SIZE - 1u) &
           ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
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
    uintptr_t end = page_align_up(virtual_addr + size_bytes);

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

    memcpy(
        (void *)copy->user_dest,
        copy->kernel_src,
        copy->size
    );
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
```

---

# 7. Update `kernel/usermode.c`

The user entry point is no longer always the beginning of the code page.

Replace:

```c
x86_enter_user_mode(
    (uint32_t)process->user_code_base,
    (uint32_t)process->user_stack_top
);
```

with:

```c
x86_enter_user_mode(
    (uint32_t)process->user_entry,
    (uint32_t)process->user_stack_top
);
```

The full function should look like this:

```c
void usermode_enter_current_process(void) {
    process_t *process = process_current();

    if (process == 0) {
        kernel_panic("usermode entry without process");
    }

    if (process->user_entry == 0 || process->user_stack_top == 0) {
        kernel_panic("usermode entry missing entry point or stack");
    }

    tss_set_kernel_stack(current_thread_kernel_stack_top());

    x86_enter_user_mode(
        (uint32_t)process->user_entry,
        (uint32_t)process->user_stack_top
    );

    kernel_panic("x86_enter_user_mode returned unexpectedly");
}
```

---

# 8. Add `kernel/toyexe.c`

```c
// kernel/toyexe.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/process.h"
#include "kernel/string.h"
#include "kernel/toyexe.h"

static uint32_t align_up_page(uint32_t value) {
    return (value + PMM_PAGE_SIZE - 1u) &
           ~(PMM_PAGE_SIZE - 1u);
}

static int validate_header(
    const toyexe_header_t *header,
    uint32_t file_size
) {
    if (header == 0) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->magic != TOYEXE_MAGIC) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->version != TOYEXE_VERSION) {
        return TOYEXE_ERR_UNSUPPORTED;
    }

    if (header->header_size < sizeof(toyexe_header_t)) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->header_size > file_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset < header->header_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset > file_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_size == 0) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset + header->image_size < header->image_offset) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_offset + header->image_size > file_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->entry_offset >= header->image_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_size + header->bss_size < header->image_size) {
        return TOYEXE_ERR_INVALID;
    }

    if (header->image_size + header->bss_size > 1024u * 1024u) {
        return TOYEXE_ERR_TOO_LARGE;
    }

    if (header->stack_size > 1024u * 1024u) {
        return TOYEXE_ERR_TOO_LARGE;
    }

    return TOYEXE_OK;
}

int toyexe_load_process(
    process_t *process,
    const uint8_t *image,
    uint32_t image_size
) {
    if (process == 0 || image == 0 || image_size < sizeof(toyexe_header_t)) {
        return TOYEXE_ERR_INVALID;
    }

    const toyexe_header_t *header = (const toyexe_header_t *)image;

    int rc = validate_header(header, image_size);

    if (rc != TOYEXE_OK) {
        return rc;
    }

    uint32_t runtime_size = header->image_size + header->bss_size;

    if (runtime_size == 0) {
        return TOYEXE_ERR_INVALID;
    }

    uint32_t stack_size = header->stack_size;

    if (stack_size == 0) {
        stack_size = TOYEXE_DEFAULT_STACK_SIZE;
    }

    stack_size = align_up_page(stack_size);

    uintptr_t user_base = TOYEXE_USER_BASE;
    uintptr_t user_stack_top = TOYEXE_DEFAULT_STACK_TOP;
    uintptr_t user_stack_base = user_stack_top - stack_size;

    if (process_map_user_region(process, user_base, runtime_size) != 0) {
        return TOYEXE_ERR_LOAD_FAILED;
    }

    if (process_map_user_region(process, user_stack_base, stack_size) != 0) {
        return TOYEXE_ERR_LOAD_FAILED;
    }

    const uint8_t *payload = image + header->image_offset;

    if (process_copy_to_user_init(
            process,
            user_base,
            payload,
            header->image_size
        ) != 0) {
        return TOYEXE_ERR_LOAD_FAILED;
    }

    if (header->bss_size != 0) {
        if (process_zero_user_init(
                process,
                user_base + header->image_size,
                header->bss_size
            ) != 0) {
            return TOYEXE_ERR_LOAD_FAILED;
        }
    }

    process->user_code_base = user_base;

    process_set_user_entry(
        process,
        user_base + header->entry_offset
    );

    process_set_user_stack(
        process,
        user_stack_base,
        user_stack_top
    );

    console_write("TOYEXE: loaded image bytes=");
    console_write_u32_dec(header->image_size);
    console_write(" bss=");
    console_write_u32_dec(header->bss_size);
    console_write(" entry=");
    console_write_hex32((uint32_t)(user_base + header->entry_offset));
    console_putc('\n');

    return TOYEXE_OK;
}

process_t *toyexe_create_process(
    const char *name,
    const uint8_t *image,
    uint32_t image_size
) {
    process_t *process = process_create_empty(name);

    int rc = toyexe_load_process(process, image, image_size);

    if (rc != TOYEXE_OK) {
        console_write("TOYEXE: load failed rc=");
        console_write_u32_dec((uint32_t)(-rc));
        console_putc('\n');

        kernel_panic("TOYEXE process load failed");
    }

    process_start_user(process);

    return process;
}
```

---

# 9. Add a TOYEXE test image builder in `kernel/process.c`

Now we replace the raw `process_test_once()` program builder with one that creates a TOYEXE image.

Add these constants near the bottom of `process.c`:

```c
#define DEMO_IMAGE_SIZE 256u
#define DEMO_BSS_SIZE   64u

#define DEMO_PROMPT_VA  (TOYEXE_USER_BASE + 0xA0u)
#define DEMO_PREFIX_VA  (TOYEXE_USER_BASE + 0xA8u)
#define DEMO_NEWLINE_VA (TOYEXE_USER_BASE + 0xB0u)
#define DEMO_INPUT_VA   (TOYEXE_USER_BASE + DEMO_IMAGE_SIZE)

#define DEMO_INPUT_MAX 32u
```

Notice this important change:

```c
#define DEMO_INPUT_VA (TOYEXE_USER_BASE + DEMO_IMAGE_SIZE)
```

The input buffer now lives in BSS.

That means the loader must zero BSS correctly or the user program’s input buffer region will not be valid.

Now add the tiny machine-code emitter.

```c
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
```

Now add the user payload builder.

```c
static void build_stdio_demo_payload(uint8_t *program, uint32_t program_size) {
    memset(program, 0x90, program_size);

    uint32_t offset = 0;

    /*
     * write(1, "user> ", 6)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_PROMPT_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    /*
     * bytes = read(0, input_buffer, 32)
     */
    emit_mov_eax_imm32(program, &offset, SYS_READ);
    emit_mov_ebx_imm32(program, &offset, FD_STDIN);
    emit_mov_ecx_imm32(program, &offset, DEMO_INPUT_VA);
    emit_mov_edx_imm32(program, &offset, DEMO_INPUT_MAX);
    emit_int80(program, &offset);

    /*
     * mov esi, eax
     *
     * Save byte count returned by read().
     */
    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xC6u);

    /*
     * write(1, "echo: ", 6)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_PREFIX_VA);
    emit_mov_edx_imm32(program, &offset, 6);
    emit_int80(program, &offset);

    /*
     * write(1, input_buffer, esi)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_INPUT_VA);

    /*
     * mov edx, esi
     */
    emit_u8(program, &offset, 0x89u);
    emit_u8(program, &offset, 0xF2u);

    emit_int80(program, &offset);

    /*
     * write(1, "\n", 1)
     */
    emit_mov_eax_imm32(program, &offset, SYS_WRITE);
    emit_mov_ebx_imm32(program, &offset, FD_STDOUT);
    emit_mov_ecx_imm32(program, &offset, DEMO_NEWLINE_VA);
    emit_mov_edx_imm32(program, &offset, 1);
    emit_int80(program, &offset);

    /*
     * sleep(3)
     */
    emit_mov_eax_imm32(program, &offset, SYS_SLEEP);
    emit_mov_ebx_imm32(program, &offset, 3);
    emit_int80(program, &offset);

    /*
     * exit(9)
     */
    emit_mov_eax_imm32(program, &offset, SYS_EXIT);
    emit_mov_ebx_imm32(program, &offset, 9);
    emit_int80(program, &offset);

    /*
     * jmp $
     */
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
```

Now add the TOYEXE wrapper builder.

```c
static uint32_t build_stdio_demo_toyexe(
    uint8_t *image,
    uint32_t image_capacity
) {
    uint32_t total_size =
        sizeof(toyexe_header_t) + DEMO_IMAGE_SIZE;

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
```

Finally, replace `process_test_once()` with this version.

```c
void process_test_once(void) {
    console_writeln("Process test: starting TOYEXE user program test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    static uint8_t toyexe_image[
        sizeof(toyexe_header_t) + DEMO_IMAGE_SIZE
    ];

    uint32_t toyexe_size = build_stdio_demo_toyexe(
        toyexe_image,
        sizeof(toyexe_image)
    );

    toyexe_create_process(
        "toyexe-demo",
        toyexe_image,
        toyexe_size
    );

    /*
     * Let the user process start, print its prompt, and block in SYS_READ.
     */
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
        kernel_panic("TOYEXE process test received wrong exit code");
    }

    console_writeln("Process test: TOYEXE load/read/write/sleep/exit sanity check passed");
}
```

---

# 10. Why the input buffer moved into BSS

In Chapter 18, the input buffer lived inside the same 256-byte page image.

This chapter moves it to:

```c
DEMO_INPUT_VA = TOYEXE_USER_BASE + DEMO_IMAGE_SIZE
```

That is immediately after the copied image bytes.

The loader maps:

```text
image_size + bss_size
```

Then it copies only:

```text
image_size
```

and zeroes:

```text
bss_size
```

So the input buffer tests that BSS exists and is writable.

That is one of the first real loader responsibilities.

---

# 11. Keep `kernel/kmain.c` mostly unchanged

`kmain.c` already calls:

```c
process_test_once();
```

That call remains the same.

The only thing that changes is what the process test does internally.

The relevant part still looks like this:

```c
monitor_init();
monitor_test_once();

process_test_once();

monitor_start();
```

No new include is required unless your compiler needs `toyexe.h` directly in `kmain.c`, which it should not.

---

# 12. Update `Makefile`

Add:

```text
build/kernel/toyexe.o
```

to the object list.

The relevant object list becomes:

```make
OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/address_space.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/monitor.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/process.o \
    build/kernel/sync.o \
    build/kernel/syscall.o \
    build/kernel/terminal.o \
    build/kernel/thread.o \
    build/kernel/toyexe.o \
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the process-related greps:

```make
grep -q "Process test: starting TOYEXE user program test" build/test.log
grep -q "Address space: created process page directory" build/test.log
grep -q "TOYEXE: loaded image bytes=256 bss=64" build/test.log
grep -q "Process: created pid=1 name=toyexe-demo" build/test.log
grep -q "user>" build/test.log
grep -q "echo: toyix" build/test.log
grep -q "Syscall: process toyexe-demo pid=1 exited code 9" build/test.log
grep -q "Process test: TOYEXE load/read/write/sleep/exit sanity check passed" build/test.log
```

The process section of the test target should now contain:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting TOYEXE user program test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "TOYEXE: loaded image bytes=256 bss=64" build/test.log
	grep -q "Process: created pid=1 name=toyexe-demo" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process toyexe-demo pid=1 exited code 9" build/test.log
	grep -q "Process test: TOYEXE load/read/write/sleep/exit sanity check passed" build/test.log
```

The final echo can become:

```make
	@echo "Boot, memory, heap, sync, monitor, address-space, and TOYEXE loader smoke test passed."
```

---

# 13. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 20 checks passed."
```

---

# 14. Expected output

A successful boot should include:

```text
Process test: starting TOYEXE user program test
Address space: created process page directory
TOYEXE: loaded image bytes=256 bss=64 entry=0x40100000
Thread: created toyexe-demo id=...
Process: created pid=1 name=toyexe-demo
user> toyix
echo: toyix
Syscall: process toyexe-demo pid=1 exited code 9
Threads: reaping zombie toyexe-demo id=...
Process test: TOYEXE load/read/write/sleep/exit sanity check passed
```

This proves:

```text
TOYEXE header validated
process address space created
image page mapped
stack page mapped
image bytes copied
BSS zeroed
entry point used
user process ran
read/write/sleep/exit syscalls worked
```

That is exactly the loader boundary we wanted.

---

# 15. Common failures

## Failure: `TOYEXE process load failed`

Print the return code from `toyexe_load_process()` and check:

```text
magic
version
header_size
image_offset
image_size
entry_offset
```

The most likely mistakes are:

```text
wrong magic value
image_offset smaller than header_size
entry_offset beyond image_size
image_size larger than provided file buffer
```

## Failure: page fault at `0x40100100`

That address is the BSS input buffer:

```c
DEMO_INPUT_VA = TOYEXE_USER_BASE + DEMO_IMAGE_SIZE
```

Likely cause:

```text
runtime_size mapped only image_size, not image_size + bss_size
```

The loader must map:

```c
runtime_size = header->image_size + header->bss_size;
```

not only:

```c
header->image_size
```

## Failure: `echo: toyix` missing

Likely causes:

```text
SYS_READ did not copy into DEMO_INPUT_VA
BSS page/region was not mapped user-accessible
terminal input injection did not send newline
process address space was not active during syscall
```

Check:

```c
keyboard_debug_inject_char('\n');
```

and confirm `DEMO_INPUT_VA` lies inside the mapped runtime region.

## Failure: kernel crashes while copying the TOYEXE payload

The copy must happen while temporarily switched to the process address space:

```c
address_space_switch(process->address_space);
memcpy((void *)user_dest, kernel_src, size);
address_space_switch(old_space);
```

If the kernel tries to copy to `0x40100000` while still using the kernel address space, that address may be unmapped.

## Failure: test works once, but a second process load fails

This chapter still does not fully reclaim process address spaces or user pages after exit.

For repeated process creation, you may need more physical memory, or you need to implement process teardown.

That is a future chapter.

---

# 16. What this chapter achieved

Before this chapter:

```text
process creation directly copied raw machine code
```

After this chapter:

```text
TOYEXE header
  ↓
TOYEXE loader
  ↓
process address space
  ↓
mapped image
  ↓
zeroed BSS
  ↓
mapped stack
  ↓
entry point
  ↓
user process
```

This is the important architectural separation:

```text
process.c
  manages process objects

toyexe.c
  knows executable format details

usermode.c
  performs ring-3 transition

syscall.c
  handles user/kernel requests
```

That is much cleaner.

---

# 17. Design limitations

TOYEXE is intentionally tiny.

It does not support:

```text
multiple load segments
read-only text
separate data permissions
relocations
symbols
debug info
dynamic linking
shared libraries
arguments
environment
file-backed loading
```

It is also still loaded from an in-kernel byte array.

But it gives us the loader shape:

```text
validate
map
copy
zero
enter
```

That is the same shape an ELF loader will use.

---

# 18. Resources

- [Chapter 20 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_20)
- [Chapter 20 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_20)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki: Executable and Linkable Format](https://wiki.osdev.org/ELF)
- [OSDev Wiki: Paging](https://wiki.osdev.org/Paging)

---

# 19. Closure

TOYEXE gives Toyix a clean loader boundary before ELF. The kernel now validates an executable header, maps image and stack pages, copies initialized bytes, zeros BSS, and enters user mode through a loader-provided entry point.

Happy Coding!
