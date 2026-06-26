# Chapter 17 — Minimal Processes, User Memory Copying, and More Robust Syscalls

In Chapter 16, we proved the biggest privilege milestone so far:

```text
ring 0 kernel
  ↓
IRET into ring 3
  ↓
user code executes
  ↓
int 0x80
  ↓
kernel syscall handler
```

But the user-mode test was still mostly a raw thread experiment. The kernel did not yet have a real object representing a user program.

This chapter adds the first **process abstraction**.

A process will own:

```text
PID
name
main thread
exit state
exit code
user code mapping
user stack mapping
```

We will also add safer user-memory access helpers:

```c
copy_from_user()
copy_to_user()
user_string_length()
```

and improve syscalls from:

```text
SYS_PUTC
SYS_EXIT
```

to:

```text
SYS_WRITE
SYS_SLEEP
SYS_EXIT
```

Intel’s manuals describe the IA-32 operating-system environment, including memory management, protection, task management, and interrupt/exception handling. Those are the architectural foundations behind user/supervisor page checks, privilege transitions, and syscall entry through an interrupt gate. ([Intel][1])

---

# 1. What this chapter adds

Add:

```text
include/kernel/
├── process.h
└── usercopy.h

kernel/
├── process.c
└── usercopy.c
```

Modify:

```text
arch/x86/vmm.h
arch/x86/vmm.c
include/kernel/vmem.h
kernel/vmem.c
include/kernel/thread.h
kernel/thread.c
include/kernel/syscall.h
kernel/syscall.c
include/kernel/usermode.h
kernel/usermode.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

The new milestone output will look like:

```text
Process test: starting user process syscall test
Process: created pid=1 name=user-demo
User process says hello through SYS_WRITE
Syscall: process user-demo pid=1 exited code 7
Process test: user process syscall/write/sleep/exit sanity check passed
```

---

# 2. Why introduce `process_t` now?

A thread is an execution stream.

A process is an ownership container.

Right now the difference is small, but it will become critical.

A future process will own:

```text
address space
threads
file descriptor table
current directory
signals/events
exit status
credentials
resource limits
```

For this tutorial kernel, we start small:

```text
process_t owns one user thread and a few user pages
```

That gives us a place to attach user-mode state without stuffing everything into `thread_t`.

---

# 3. Add `include/kernel/process.h`

```c
// include/kernel/process.h
#ifndef TOYIX_KERNEL_PROCESS_H
#define TOYIX_KERNEL_PROCESS_H

#include <stdint.h>

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

    struct thread *main_thread;

    uint32_t exit_code;
    int exited;

    uintptr_t user_code_base;
    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
} process_t;

void process_init_system(void);

process_t *process_create_user(
    const char *name,
    const uint8_t *program,
    uint32_t program_size
);

process_t *process_current(void);

void process_exit_current(uint32_t exit_code);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

void process_test_once(void);

#endif
```

## Why only one thread per process for now?

Because one thread is enough to prove:

```text
process object exists
thread is attached to process
syscalls can identify current process
process exit status is recorded
```

Multi-threaded processes can come later.

---

# 4. Update `include/kernel/thread.h`

Add a forward declaration and process pointer.

Near the top:

```c
struct process;
```

Inside `thread_t`, add:

```c
struct process *process;
```

Add these declarations:

```c
void thread_set_process(thread_t *thread, struct process *process);
struct process *thread_get_process(thread_t *thread);
```

Here is the relevant updated portion:

```c
struct process;

typedef struct thread {
    uint32_t magic;
    uint32_t id;

    const char *name;
    thread_state_t state;

    thread_context_t context;

    void *stack_base;
    uint32_t stack_size;

    thread_entry_t entry;
    void *arg;

    struct process *process;

    uint32_t wake_tick;

    struct thread *next;
    struct thread *prev;
} thread_t;
```

---

# 5. Update `kernel/thread.c`

Whenever a thread is initialized, set:

```c
thread->process = 0;
```

In `thread_create_internal()`, add:

```c
thread->process = 0;
```

In `threading_init()`, for the bootstrap thread:

```c
bootstrap_thread.process = 0;
```

Then add these functions near `thread_current()`:

```c
void thread_set_process(thread_t *thread, struct process *process) {
    validate_thread(thread);
    thread->process = process;
}

struct process *thread_get_process(thread_t *thread) {
    validate_thread(thread);
    return thread->process;
}
```

## Why store process on the thread?

When a syscall runs, the kernel can find the process through:

```c
thread_current()->process
```

That lets the syscall layer know who called it.

Later, when we have many processes and many threads, this relationship becomes foundational.

---

# 6. Update `arch/x86/vmm.h`

Add a function to retrieve raw page flags.

```c
// arch/x86/vmm.h
#ifndef TOYIX_ARCH_X86_VMM_H
#define TOYIX_ARCH_X86_VMM_H

#include <stdint.h>

#define VMM_OK 0
#define VMM_ERR_INVALID -1
#define VMM_ERR_NO_MEMORY -2
#define VMM_ERR_ALREADY_MAPPED -3
#define VMM_ERR_NOT_MAPPED -4

void vmm_init(void);

int vmm_map_page(
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
);

int vmm_unmap_page(uintptr_t virtual_addr);

uintptr_t vmm_get_physical(uintptr_t virtual_addr);
uint32_t vmm_get_flags(uintptr_t virtual_addr);

void vmm_test_once(void);

#endif
```

---

# 7. Update `arch/x86/vmm.c`

Add this function after `vmm_get_physical()`.

```c
uint32_t vmm_get_flags(uintptr_t virtual_addr) {
    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    page_directory_entry_t pde = kernel_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    page_table_entry_t *table = table_from_pde(pde);
    page_table_entry_t pte = table[tab];

    if ((pte & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    return pte & X86_PAGE_FLAGS_MASK;
}
```

## Why flags matter

`vmm_get_physical()` tells us whether an address is mapped.

But for user-copy safety, we also need to know whether the page is mapped as user-accessible:

```text
page present?
page user-accessible?
```

The Intel architecture uses page-table permission bits as part of memory protection; Volume 3 documents the operating-system support environment where paging and protection are enforced. ([Intel][1])

---

# 8. Update `include/kernel/vmem.h`

Add:

```c
uint32_t vmem_get_flags(uintptr_t virtual_addr);
int vmem_is_user_accessible(uintptr_t virtual_addr);
```

Full updated header:

```c
// include/kernel/vmem.h
#ifndef TOYIX_KERNEL_VMEM_H
#define TOYIX_KERNEL_VMEM_H

#include <stdint.h>

#define VMEM_OK 0
#define VMEM_ERR_INVALID -1
#define VMEM_ERR_NO_MEMORY -2
#define VMEM_ERR_ALREADY_MAPPED -3
#define VMEM_ERR_NOT_MAPPED -4

#define VMEM_FLAG_WRITABLE 0x00000001u
#define VMEM_FLAG_USER     0x00000002u

void vmem_init(void);

int vmem_map_page(
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
);

int vmem_unmap_page(uintptr_t virtual_addr);

uintptr_t vmem_get_physical(uintptr_t virtual_addr);
uint32_t vmem_get_flags(uintptr_t virtual_addr);
int vmem_is_user_accessible(uintptr_t virtual_addr);

void vmem_test_once(void);

#endif
```

---

# 9. Update `kernel/vmem.c`

Add this helper:

```c
static uint32_t vmem_from_arch_flags(uint32_t arch_flags) {
    uint32_t flags = 0;

    if ((arch_flags & X86_PAGE_WRITABLE) != 0) {
        flags |= VMEM_FLAG_WRITABLE;
    }

    if ((arch_flags & X86_PAGE_USER) != 0) {
        flags |= VMEM_FLAG_USER;
    }

    return flags;
}
```

Then add:

```c
uint32_t vmem_get_flags(uintptr_t virtual_addr) {
    return vmem_from_arch_flags(vmm_get_flags(virtual_addr));
}

int vmem_is_user_accessible(uintptr_t virtual_addr) {
    return (vmem_get_flags(virtual_addr) & VMEM_FLAG_USER) != 0;
}
```

## Why translate flags here?

The process and syscall layers should not know that x86 calls the user page bit:

```c
X86_PAGE_USER
```

They should only know the generic kernel meaning:

```c
VMEM_FLAG_USER
```

This keeps the architecture boundary cleaner.

---

# 10. Add `include/kernel/usercopy.h`

```c
// include/kernel/usercopy.h
#ifndef TOYIX_KERNEL_USERCOPY_H
#define TOYIX_KERNEL_USERCOPY_H

#include <stddef.h>
#include <stdint.h>

#define USERCOPY_OK 0
#define USERCOPY_ERR_FAULT -1
#define USERCOPY_ERR_TOO_LONG -2

int copy_from_user(void *kernel_dest, uintptr_t user_src, size_t size);
int copy_to_user(uintptr_t user_dest, const void *kernel_src, size_t size);

int user_string_length(
    uintptr_t user_str,
    size_t max_len,
    size_t *length_out
);

#endif
```

---

# 11. Add `kernel/usercopy.c`

```c
// kernel/usercopy.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/string.h"
#include "kernel/usercopy.h"
#include "kernel/vmem.h"

static int user_range_accessible(uintptr_t user_addr, size_t size) {
    if (size == 0) {
        return 1;
    }

    if (user_addr + size < user_addr) {
        return 0;
    }

    uintptr_t start = user_addr;
    uintptr_t end = user_addr + size - 1u;

    uintptr_t page = start & ~(uintptr_t)0xFFFu;

    while (page <= end) {
        if (!vmem_is_user_accessible(page)) {
            return 0;
        }

        if (page > 0xFFFFFFFFu - 0x1000u) {
            break;
        }

        page += 0x1000u;
    }

    return 1;
}

int copy_from_user(void *kernel_dest, uintptr_t user_src, size_t size) {
    if (kernel_dest == 0 && size != 0) {
        return USERCOPY_ERR_FAULT;
    }

    if (!user_range_accessible(user_src, size)) {
        return USERCOPY_ERR_FAULT;
    }

    memcpy(kernel_dest, (const void *)user_src, size);
    return USERCOPY_OK;
}

int copy_to_user(uintptr_t user_dest, const void *kernel_src, size_t size) {
    if (kernel_src == 0 && size != 0) {
        return USERCOPY_ERR_FAULT;
    }

    if (!user_range_accessible(user_dest, size)) {
        return USERCOPY_ERR_FAULT;
    }

    memcpy((void *)user_dest, kernel_src, size);
    return USERCOPY_OK;
}

int user_string_length(
    uintptr_t user_str,
    size_t max_len,
    size_t *length_out
) {
    if (length_out == 0) {
        return USERCOPY_ERR_FAULT;
    }

    for (size_t i = 0; i < max_len; ++i) {
        char ch;

        if (copy_from_user(&ch, user_str + i, 1) != USERCOPY_OK) {
            return USERCOPY_ERR_FAULT;
        }

        if (ch == '\0') {
            *length_out = i;
            return USERCOPY_OK;
        }
    }

    return USERCOPY_ERR_TOO_LONG;
}
```

## Why copy helpers matter

A syscall handler must not blindly trust a user pointer.

Bad user code may pass:

```text
0x00000000
kernel address
unmapped address
string without terminator
buffer crossing into unmapped memory
```

`copy_from_user()` and `copy_to_user()` are the boundary checks that keep syscall code from becoming a crash machine.

This is not fully hardened yet, but it establishes the right pattern.

---

# 12. Update `include/kernel/syscall.h`

Replace it with:

```c
// include/kernel/syscall.h
#ifndef TOYIX_KERNEL_SYSCALL_H
#define TOYIX_KERNEL_SYSCALL_H

#include "arch/x86/interrupts.h"

#define SYS_PUTC  1u
#define SYS_EXIT  2u
#define SYS_WRITE 3u
#define SYS_SLEEP 4u

void syscall_handler(interrupt_frame_t *frame);

#endif
```

We keep `SYS_PUTC` for compatibility with the Chapter 16 test, but the new user process will use `SYS_WRITE`.

---

# 13. Replace `kernel/syscall.c`

```c
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
```

## Why `SYS_SLEEP` is interesting

`SYS_SLEEP` proves user code can call into the kernel and block the current user process thread.

That means:

```text
user mode
  ↓ syscall
kernel scheduler blocks thread
  ↓ timer wakes it
return to user mode
```

This is a real OS behavior.

---

# 14. Replace `include/kernel/usermode.h`

```c
// include/kernel/usermode.h
#ifndef TOYIX_KERNEL_USERMODE_H
#define TOYIX_KERNEL_USERMODE_H

#include <stdint.h>

void usermode_enter_current_process(void);

#endif
```

---

# 15. Replace `kernel/usermode.c`

```c
// kernel/usermode.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"

extern void x86_enter_user_mode(uint32_t user_eip, uint32_t user_esp);

static uint32_t current_thread_kernel_stack_top(void) {
    thread_t *self = thread_current();

    if (self == 0 || self->stack_base == 0) {
        kernel_panic("user mode entry requires current thread stack");
    }

    return (uint32_t)((uintptr_t)self->stack_base + self->stack_size);
}

void usermode_enter_current_process(void) {
    process_t *process = process_current();

    if (process == 0) {
        kernel_panic("usermode entry without process");
    }

    tss_set_kernel_stack(current_thread_kernel_stack_top());

    x86_enter_user_mode(
        (uint32_t)process->user_code_base,
        (uint32_t)process->user_stack_top
    );

    kernel_panic("x86_enter_user_mode returned unexpectedly");
}
```

---

# 16. Add `kernel/process.c`

```c
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

/*
 * User program:
 *
 *   SYS_WRITE("User process says hello through SYS_WRITE\n")
 *   SYS_SLEEP(3)
 *   SYS_EXIT(7)
 */
static const uint8_t user_process_demo[] = {
    /*
     * mov eax, SYS_WRITE
     * mov ebx, message_addr
     * mov ecx, message_len
     * int 0x80
     */
    0xB8, SYS_WRITE, 0x00, 0x00, 0x00,
    0xBB, 0x40, 0x00, 0x10, 0x40,
    0xB9, 0x2Au, 0x00, 0x00, 0x00,
    0xCD, 0x80,

    /*
     * mov eax, SYS_SLEEP
     * mov ebx, 3
     * int 0x80
     */
    0xB8, SYS_SLEEP, 0x00, 0x00, 0x00,
    0xBB, 0x03,      0x00, 0x00, 0x00,
    0xCD, 0x80,

    /*
     * mov eax, SYS_EXIT
     * mov ebx, 7
     * int 0x80
     */
    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0xBB, 0x07,     0x00, 0x00, 0x00,
    0xCD, 0x80,

    /*
     * jmp $
     */
    0xEB, 0xFE,

    /*
     * Padding to offset 0x40.
     */
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90,

    /*
     * Offset 0x40:
     * "User process says hello through SYS_WRITE\n"
     *
     * Length is 42 bytes.
     */
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
```

## Important note about the machine-code message address

The user program uses:

```text
message address = 0x40100040
```

because:

```text
USER_PROCESS_CODE_VA = 0x40100000
message offset        = 0x40
```

This is crude, but good enough for one page of test code.

A real executable loader will relocate or load segments properly.

---

# 17. Update `kernel/kmain.c`

Add:

```c
#include "kernel/process.h"
```

Then call:

```c
process_init_system();
```

after threading is initialized.

Call:

```c
process_test_once();
```

after the old user-mode test location.

Since Chapter 17 replaces `usermode_test_once()`, remove that call.

Relevant section:

```c
threading_init();
process_init_system();
thread_test_once();
```

Later:

```c
monitor_init();
monitor_test_once();

process_test_once();

monitor_start();
```

## Full relevant flow

```text
threading_init()
process_init_system()
thread tests
...
terminal tests
monitor tests
process test
start monitor
idle forever
```

---

# 18. Update `Makefile`

Add:

```text
build/kernel/process.o
build/kernel/usercopy.o
```

Keep:

```text
build/kernel/usermode.o
```

because it still contains the ring-3 entry helper.

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
    build/arch/x86/paging_asm.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/sched_interrupt.o \
    build/arch/x86/syscall.o \
    build/arch/x86/user_enter.o \
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/idle.o \
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
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update test greps.

Remove Chapter 16-specific greps:

```make
grep -q "User mode test: preparing ring 3 program" build/test.log
grep -q "User mode test: ring 3 syscall/exit sanity check passed" build/test.log
```

Add:

```make
grep -q "Process: process table initialized" build/test.log
grep -q "Process test: starting user process syscall test" build/test.log
grep -q "Process: created pid=1 name=user-demo" build/test.log
grep -q "User process says hello through SYS_WRITE" build/test.log
grep -q "Syscall: process user-demo pid=1 exited code 7" build/test.log
grep -q "Process test: user process syscall/write/sleep/exit sanity check passed" build/test.log
```

---

# 19. Expected output

A successful boot should include:

```text
Process: process table initialized
...
Process test: starting user process syscall test
Process: created pid=1 name=user-demo
User process says hello through SYS_WRITE
Syscall: process user-demo pid=1 exited code 7
Threads: reaping zombie user-demo id=...
Process test: user process syscall/write/sleep/exit sanity check passed
Monitor: monitor thread started
```

That proves:

```text
process object created
user code mapped
user stack mapped
thread attached to process
ring-3 code ran
SYS_WRITE copied from user memory
SYS_SLEEP blocked user thread
SYS_EXIT recorded process exit status
zombie thread was reaped
```

---

# 20. Common failures

## Failure: `SYS_WRITE` prints garbage

Check the hardcoded message address:

```text
0x40100040
```

and make sure the message really begins at offset `0x40` inside `user_process_demo`.

If you change the machine-code bytes before the message, update the offset.

## Failure: `copy_from_user()` fails

Likely causes:

```text
code page not mapped VMEM_FLAG_USER
wrong message pointer
message crosses into an unmapped page
vmem_get_flags() not implemented correctly
```

Check:

```c
vmem_map_page(..., VMEM_FLAG_WRITABLE | VMEM_FLAG_USER)
```

and:

```c
vmem_is_user_accessible(page)
```

## Failure: process exits but test never completes

Check:

```c
last_exit_seen = 1;
```

inside `process_exit_current()`.

Also make sure `SYS_EXIT` calls:

```c
process_exit_current(exit_code);
thread_exit();
```

## Failure: `SYS_SLEEP` never wakes

That means the user thread blocked but the timer/sleep queue did not wake it.

Check:

```c
thread_sleep_ticks(ticks);
```

inside the syscall handler.

The user process test calls `SYS_SLEEP(3)`, so it requires interrupts and PIT scheduling to already be active.

---

# 21. What this chapter achieved

We now have the beginning of a process model:

```text
process_t
  ↓
main user thread
  ↓
user code page
  ↓
user stack page
  ↓
syscalls
  ↓
exit status
```

The syscall layer now does a more realistic thing:

```text
validate user pointer
copy from user memory
perform kernel operation
return result in EAX
```

This is the foundation for:

```text
file descriptors
read/write syscalls
ELF loading
per-process address spaces
waitpid
exec
fork-like cloning, much later
```

---

# 22. Design limitations

This is still not a Unix process model.

Current limitations:

```text
all processes share the same page directory
user pages are mapped globally
no per-process address spaces
no process list
no process wait queue
no parent/child relationship
no file descriptors
no ELF loader
no user heap
no argument vector
```

That is expected.

The next big architectural step is per-process address spaces. But before that, it is useful to add a simple file-like abstraction for stdout and keyboard input, because it makes user programs more interesting.

---

# 23. Next chapter

The next chapter can go in either of two directions.

The practical path:

```text
file descriptors 0/1/2
SYS_READ
SYS_WRITE using fd
user program reads keyboard and echoes input
```

The deeper memory path:

```text
per-process page directory
kernel mappings cloned into each process
CR3 switch on context switch
separate user address spaces
```

I recommend the practical path first. It will give us a tiny user-mode console program before we tackle address-space isolation.

[1]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel® 64 and IA-32 Architectures"

---

# 24. Resources

- [Chapter 17 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_17)
- [Chapter 17 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_17)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki: System Calls](https://wiki.osdev.org/System_Calls)
- [OSDev Wiki: User Space](https://wiki.osdev.org/User_Space)

---

# 25. Closure

The kernel now has a process object for user programs, checked user-memory copying, and more realistic syscalls for writing, sleeping, and exiting. That gives later chapters a concrete place to attach file descriptors, address spaces, wait state, and executable loading.

Happy Coding!
