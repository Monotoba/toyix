# Chapter 19 — Per-Process Address Spaces and CR3 Switching

In Chapter 18, user programs gained a much more realistic syscall interface:

```text
read(0, buffer, len)
write(1, buffer, len)
sleep(ticks)
exit(code)
```

But all user processes still shared the same page directory.

That means every process saw the same user mappings:

```text
process A code at 0x40100000
process B code at 0x40100000
same address space
same mappings
```

This chapter gives each process its own address space.

The new model becomes:

```text
kernel mappings
  shared by every address space

user mappings
  private to each process
```

When the scheduler switches between processes, it will also switch `CR3`.

That is the register that points to the active page directory on 32-bit x86.

---

# 1. Important design limitation

Our kernel is still identity-mapped at low physical memory.

That means every process address space must still contain supervisor-only mappings for the kernel’s low memory region:

```text
0x00000000 - 0x00FFFFFF  shared supervisor mappings
```

This is not ideal long-term.

A more mature kernel usually moves itself to a higher-half virtual address range, for example:

```text
0xC0000000 - 0xFFFFFFFF  kernel
0x00000000 - 0xBFFFFFFF  user
```

We are not doing the full higher-half conversion yet.

For this chapter, we use an intermediate design:

```text
low 16 MiB identity mapping   shared, supervisor only
high kernel heap/VMM region   shared, supervisor only
user program region           private, user-accessible
```

This is enough to prove per-process page directories and `CR3` switching.

---

# 2. What this chapter adds

Add:

```text
include/kernel/
└── address_space.h

kernel/
└── address_space.c
```

Modify:

```text
arch/x86/paging.h
kernel/thread.h
kernel/thread.c
kernel/process.h
kernel/process.c
kernel/usercopy.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

The milestone output will look like:

```text
Address space: kernel address space registered
Process: process table initialized
Process test: starting isolated address-space syscall test
Thread: created stdio-demo id=...
Address space: created process page directory
Process: created pid=1 name=stdio-demo
user>
toyix
echo: toyix
Syscall: process stdio-demo pid=1 exited code 9
Process test: isolated address-space syscall sanity check passed
```

---

# 3. Update `arch/x86/paging.h`

We need to expose a few paging helpers that already exist in assembly.

Replace or extend `paging.h` so it includes these declarations:

```c
// arch/x86/paging.h
#ifndef TOYIX_ARCH_X86_PAGING_H
#define TOYIX_ARCH_X86_PAGING_H

#include <stdint.h>

#define X86_PAGE_SIZE 4096u

#define X86_PAGE_PRESENT      0x001u
#define X86_PAGE_WRITABLE     0x002u
#define X86_PAGE_USER         0x004u
#define X86_PAGE_WRITETHROUGH 0x008u
#define X86_PAGE_NOCACHE      0x010u
#define X86_PAGE_ACCESSED     0x020u
#define X86_PAGE_DIRTY        0x040u
#define X86_PAGE_GLOBAL       0x100u

#define X86_PAGE_FRAME_MASK   0xFFFFF000u
#define X86_PAGE_FLAGS_MASK   0x00000FFFu

void paging_init(void);
int paging_is_enabled(void);
void paging_test_identity_mapping(void);

uint32_t *paging_get_kernel_directory(void);

void paging_load_directory(uint32_t physical_addr);
void paging_reload_cr3(void);
void paging_invalidate_page(uintptr_t virtual_addr);

#endif
```

These functions let the new address-space layer switch page directories and invalidate mappings.

---

# 4. Add `include/kernel/address_space.h`

```c
// include/kernel/address_space.h
#ifndef TOYIX_KERNEL_ADDRESS_SPACE_H
#define TOYIX_KERNEL_ADDRESS_SPACE_H

#include <stdint.h>

#define ADDRESS_SPACE_FLAG_WRITABLE 0x00000001u
#define ADDRESS_SPACE_FLAG_USER     0x00000002u

#define ADDRESS_SPACE_USER_BASE 0x01000000u
#define ADDRESS_SPACE_USER_TOP  0xC0000000u

typedef struct address_space {
    uint32_t magic;

    uint32_t *page_directory;
    uintptr_t page_directory_physical;

    uint32_t user_page_count;
} address_space_t;

void address_space_init(void);

address_space_t *address_space_kernel(void);
address_space_t *address_space_current(void);

address_space_t *address_space_create(void);

void address_space_switch(address_space_t *space);

int address_space_map_page(
    address_space_t *space,
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
);

int address_space_unmap_page(
    address_space_t *space,
    uintptr_t virtual_addr
);

uintptr_t address_space_get_physical(
    address_space_t *space,
    uintptr_t virtual_addr
);

uint32_t address_space_get_flags(
    address_space_t *space,
    uintptr_t virtual_addr
);

#endif
```

## Why define a user range?

For this chapter, user pages must live between:

```text
0x01000000 and 0xBFFFFFFF
```

We avoid the first 16 MiB because our kernel still depends on low identity mappings there.

We avoid `0xC0000000` and above because that area is reserved for shared kernel mappings.

---

# 5. Add `kernel/address_space.c`

```c
// kernel/address_space.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/paging.h"
#include "kernel/address_space.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"

#define ADDRESS_SPACE_MAGIC 0x41535043u

#define PAGE_DIRECTORY_ENTRIES 1024u
#define PAGE_TABLE_ENTRIES     1024u

#define PAGE_TABLE_BOOTSTRAP_LIMIT 0x01000000u

#define KERNEL_HIGH_BASE 0xC0000000u
#define LOW_IDENTITY_SHARED_BYTES 0x01000000u

static address_space_t kernel_space;
static address_space_t *current_space;

static uint32_t directory_index(uintptr_t virtual_addr) {
    return (uint32_t)((virtual_addr >> 22) & 0x3FFu);
}

static uint32_t table_index(uintptr_t virtual_addr) {
    return (uint32_t)((virtual_addr >> 12) & 0x3FFu);
}

static uint32_t to_arch_flags(uint32_t flags) {
    uint32_t arch_flags = 0;

    if ((flags & ADDRESS_SPACE_FLAG_WRITABLE) != 0) {
        arch_flags |= X86_PAGE_WRITABLE;
    }

    if ((flags & ADDRESS_SPACE_FLAG_USER) != 0) {
        arch_flags |= X86_PAGE_USER;
    }

    return arch_flags;
}

static uint32_t from_arch_flags(uint32_t arch_flags) {
    uint32_t flags = 0;

    if ((arch_flags & X86_PAGE_WRITABLE) != 0) {
        flags |= ADDRESS_SPACE_FLAG_WRITABLE;
    }

    if ((arch_flags & X86_PAGE_USER) != 0) {
        flags |= ADDRESS_SPACE_FLAG_USER;
    }

    return flags;
}

static void validate_space(address_space_t *space) {
    if (space == 0) {
        kernel_panic("address_space: null address space");
    }

    if (space->magic != ADDRESS_SPACE_MAGIC) {
        kernel_panic("address_space: magic mismatch");
    }

    if (space->page_directory == 0 ||
        space->page_directory_physical == 0) {
        kernel_panic("address_space: missing page directory");
    }
}

static int is_page_aligned(uintptr_t value) {
    return (value & (X86_PAGE_SIZE - 1u)) == 0;
}

static int user_virtual_allowed(uintptr_t virtual_addr) {
    return virtual_addr >= ADDRESS_SPACE_USER_BASE &&
           virtual_addr < ADDRESS_SPACE_USER_TOP;
}

static uint32_t *table_from_pde(uint32_t pde) {
    return (uint32_t *)(uintptr_t)(pde & X86_PAGE_FRAME_MASK);
}

static void sync_kernel_mappings(address_space_t *space) {
    uint32_t *kernel_dir = paging_get_kernel_directory();
    uint32_t *dir = space->page_directory;

    /*
     * Share the low identity mappings needed by the current low-mapped kernel.
     *
     * These mappings must remain supervisor-only.
     */
    uint32_t low_shared_entries =
        LOW_IDENTITY_SHARED_BYTES / (4u * 1024u * 1024u);

    for (uint32_t i = 0; i < low_shared_entries; ++i) {
        dir[i] = kernel_dir[i];
    }

    /*
     * Share the high kernel region.
     *
     * This includes the kernel heap and future higher-half kernel mappings.
     */
    uint32_t high_start = directory_index(KERNEL_HIGH_BASE);

    for (uint32_t i = high_start; i < PAGE_DIRECTORY_ENTRIES; ++i) {
        dir[i] = kernel_dir[i];
    }
}

void address_space_init(void) {
    kernel_space.magic = ADDRESS_SPACE_MAGIC;
    kernel_space.page_directory = paging_get_kernel_directory();
    kernel_space.page_directory_physical =
        (uintptr_t)paging_get_kernel_directory();
    kernel_space.user_page_count = 0;

    current_space = &kernel_space;

    console_writeln("Address space: kernel address space registered");
}

address_space_t *address_space_kernel(void) {
    return &kernel_space;
}

address_space_t *address_space_current(void) {
    return current_space;
}

address_space_t *address_space_create(void) {
    uintptr_t directory_phys =
        pmm_alloc_page_below(PAGE_TABLE_BOOTSTRAP_LIMIT);

    if (directory_phys == PMM_INVALID_PAGE) {
        kernel_panic("address_space_create could not allocate page directory");
    }

    uint32_t *directory = (uint32_t *)(uintptr_t)directory_phys;

    memset(directory, 0, X86_PAGE_SIZE);

    address_space_t *space =
        (address_space_t *)kmalloc(sizeof(address_space_t));

    if (space == 0) {
        kernel_panic("address_space_create could not allocate object");
    }

    space->magic = ADDRESS_SPACE_MAGIC;
    space->page_directory = directory;
    space->page_directory_physical = directory_phys;
    space->user_page_count = 0;

    sync_kernel_mappings(space);

    console_writeln("Address space: created process page directory");

    return space;
}

void address_space_switch(address_space_t *space) {
    validate_space(space);

    /*
     * Refresh shared kernel PDEs before every switch. This helps when the
     * kernel has grown or added mappings since the process address space was
     * created.
     */
    if (space != &kernel_space) {
        sync_kernel_mappings(space);
    }

    if (current_space == space) {
        return;
    }

    current_space = space;
    paging_load_directory((uint32_t)space->page_directory_physical);
}

static uint32_t *get_or_create_table(
    address_space_t *space,
    uint32_t dir_index,
    uint32_t flags
) {
    uint32_t *directory = space->page_directory;
    uint32_t pde = directory[dir_index];

    if ((pde & X86_PAGE_PRESENT) != 0) {
        return table_from_pde(pde);
    }

    uintptr_t table_phys =
        pmm_alloc_page_below(PAGE_TABLE_BOOTSTRAP_LIMIT);

    if (table_phys == PMM_INVALID_PAGE) {
        return 0;
    }

    uint32_t *table = (uint32_t *)(uintptr_t)table_phys;
    memset(table, 0, X86_PAGE_SIZE);

    uint32_t pde_flags =
        X86_PAGE_PRESENT |
        X86_PAGE_WRITABLE |
        to_arch_flags(flags);

    directory[dir_index] =
        (uint32_t)(table_phys & X86_PAGE_FRAME_MASK) | pde_flags;

    return table;
}

int address_space_map_page(
    address_space_t *space,
    uintptr_t virtual_addr,
    uintptr_t physical_addr,
    uint32_t flags
) {
    validate_space(space);

    if (!is_page_aligned(virtual_addr) ||
        !is_page_aligned(physical_addr)) {
        return -1;
    }

    if (!user_virtual_allowed(virtual_addr)) {
        return -1;
    }

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t *table = get_or_create_table(space, dir, flags);

    if (table == 0) {
        return -1;
    }

    if ((table[tab] & X86_PAGE_PRESENT) != 0) {
        return -1;
    }

    uint32_t pte_flags =
        X86_PAGE_PRESENT |
        to_arch_flags(flags);

    table[tab] =
        (uint32_t)(physical_addr & X86_PAGE_FRAME_MASK) | pte_flags;

    space->user_page_count++;

    if (space == current_space) {
        paging_invalidate_page(virtual_addr);
    }

    return 0;
}

int address_space_unmap_page(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    validate_space(space);

    if (!is_page_aligned(virtual_addr)) {
        return -1;
    }

    if (!user_virtual_allowed(virtual_addr)) {
        return -1;
    }

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t pde = space->page_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return -1;
    }

    uint32_t *table = table_from_pde(pde);

    if ((table[tab] & X86_PAGE_PRESENT) == 0) {
        return -1;
    }

    table[tab] = 0;

    if (space->user_page_count > 0) {
        space->user_page_count--;
    }

    if (space == current_space) {
        paging_invalidate_page(virtual_addr);
    }

    return 0;
}

uintptr_t address_space_get_physical(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    validate_space(space);

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t pde = space->page_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    uint32_t *table = table_from_pde(pde);
    uint32_t pte = table[tab];

    if ((pte & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    return (pte & X86_PAGE_FRAME_MASK) |
           (virtual_addr & (X86_PAGE_SIZE - 1u));
}

uint32_t address_space_get_flags(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    validate_space(space);

    uint32_t dir = directory_index(virtual_addr);
    uint32_t tab = table_index(virtual_addr);

    uint32_t pde = space->page_directory[dir];

    if ((pde & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    uint32_t *table = table_from_pde(pde);
    uint32_t pte = table[tab];

    if ((pte & X86_PAGE_PRESENT) == 0) {
        return 0;
    }

    return from_arch_flags(pte & X86_PAGE_FLAGS_MASK);
}
```

---

# 6. What is shared and what is private?

Each process gets a new page directory.

But the process page directory copies selected kernel entries from the kernel directory.

Shared:

```text
low 16 MiB identity mappings
high kernel mappings from 0xC0000000 upward
kernel heap mappings
kernel page tables for shared regions
```

Private:

```text
user code page
user stack page
future user heap pages
future mmap/load pages
```

So two processes can both use:

```text
0x40100000
```

and those virtual addresses can point to different physical pages.

That is the essential point of per-process address spaces.

---

# 7. Update `kernel/process.h`

Add the address-space pointer.

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

---

# 8. Update `kernel/thread.h`

Add suspended thread creation and process accessors.

```c
thread_t *thread_create_suspended(
    const char *name,
    thread_entry_t entry,
    void *arg
);

void thread_start(thread_t *thread);

void thread_set_process(thread_t *thread, struct process *process);
struct process *thread_get_process(thread_t *thread);
```

The relevant part should look like this:

```c
thread_t *thread_create(
    const char *name,
    thread_entry_t entry,
    void *arg
);

thread_t *thread_create_suspended(
    const char *name,
    thread_entry_t entry,
    void *arg
);

void thread_start(thread_t *thread);

void thread_set_process(thread_t *thread, struct process *process);
struct process *thread_get_process(thread_t *thread);
```

## Why suspended creation is needed

This avoids a race.

Bad sequence:

```text
thread_create()
  ↓
thread enters ready queue
  ↓
timer schedules it before process pointer is attached
```

Correct sequence:

```text
thread_create_suspended()
  ↓
attach process pointer
  ↓
thread_start()
```

That guarantees the process is fully attached before the thread can run.

---

# 9. Update `kernel/thread.c`

Add includes:

```c
#include "kernel/address_space.h"
#include "kernel/process.h"
```

When creating a thread, initialize:

```c
thread->process = 0;
```

For the bootstrap thread:

```c
bootstrap_thread.process = 0;
```

Add these helpers:

```c
void thread_set_process(thread_t *thread, struct process *process) {
    validate_thread(thread);
    thread->process = process;
}

struct process *thread_get_process(thread_t *thread) {
    validate_thread(thread);
    return thread->process;
}

thread_t *thread_create_suspended(
    const char *name,
    thread_entry_t entry,
    void *arg
) {
    return thread_create_internal(name, entry, arg, 0);
}

void thread_start(thread_t *thread) {
    if (thread == 0) {
        kernel_panic("thread_start received null thread");
    }

    irq_flags_t flags = irq_save();

    validate_thread(thread);

    if (thread->state != THREAD_NEW) {
        irq_restore(flags);
        kernel_panic("thread_start called on non-new thread");
    }

    ready_push(thread);

    irq_restore(flags);
}
```

Then adjust `thread_create_internal()` so it leaves suspended threads in `THREAD_NEW`.

Find the end of `thread_create_internal()` and make sure it behaves like this:

```c
thread_make_initial_stack(thread);

if (enqueue) {
    irq_flags_t flags = irq_save();
    ready_push(thread);
    irq_restore(flags);
}

console_write("Thread: created ");
console_write(thread->name);
console_write(" id=");
console_write_u32_dec(thread->id);
console_write(" stack=");
console_write_hex32((uint32_t)(uintptr_t)thread->stack_base);
console_putc('\n');

return thread;
```

Do **not** set a suspended thread to `READY`.

`ready_push()` will set the state when `thread_start()` is called.

---

# 10. Switch address spaces in the scheduler

Still in `kernel/thread.c`, add this helper:

```c
static uint32_t thread_kernel_stack_top(thread_t *thread) {
    if (thread == 0 || thread->stack_base == 0) {
        return 0;
    }

    return (uint32_t)((uintptr_t)thread->stack_base + thread->stack_size);
}

static address_space_t *thread_address_space(thread_t *thread) {
    if (thread == 0 || thread->process == 0) {
        return address_space_kernel();
    }

    if (thread->process->address_space == 0) {
        return address_space_kernel();
    }

    return thread->process->address_space;
}
```

Then update the end of `thread_schedule_from_interrupt()`.

The final part should look like this:

```c
next_thread->state = THREAD_RUNNING;
current_thread = next_thread;

address_space_switch(thread_address_space(next_thread));

uint32_t next_stack_top = thread_kernel_stack_top(next_thread);

if (next_stack_top != 0) {
    tss_set_kernel_stack(next_stack_top);
}

current_slice_ticks = 0;
reschedule_requested = 0;

return (uintptr_t)next_thread->context.esp;
```

## Why switch `CR3` in the scheduler?

The scheduler is where the CPU changes from one thread to another.

If the next thread belongs to a different process, the CPU must also switch to that process’s page directory.

So a context switch is now:

```text
choose next thread
  ↓
switch address space
  ↓
update TSS kernel stack
  ↓
restore next interrupt frame
  ↓
IRETD into next context
```

---

# 11. Update `kernel/usercopy.c`

Change it so user-copy checks the **current address space**, not the old global VMM mapping.

Replace the includes:

```c
#include "kernel/vmem.h"
```

with:

```c
#include "kernel/address_space.h"
```

Then replace `user_range_accessible()` with this version:

```c
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

    address_space_t *space = address_space_current();

    while (page <= end) {
        uint32_t flags = address_space_get_flags(space, page);

        if ((flags & ADDRESS_SPACE_FLAG_USER) == 0) {
            return 0;
        }

        if (page > 0xFFFFFFFFu - 0x1000u) {
            break;
        }

        page += 0x1000u;
    }

    return 1;
}
```

## Why this matters

Before this chapter, checking the global kernel VMM was enough because all processes shared one page directory.

Now that each process has its own mappings, user-copy must validate against the currently active address space.

When a user process performs a syscall, the active address space is that process’s page directory.

That is exactly what we need.

---

# 12. Update `kernel/process.c`

Add includes:

```c
#include "arch/x86/irq_state.h"
#include "kernel/address_space.h"
```

Then replace the old `map_user_page()` with address-space-aware mapping.

```c
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
```

Update `process_create_user()` so it creates the suspended process thread before the process address space is created, then maps user pages into the private address space and temporarily switches into it to copy the program.

Here is the full replacement for `process_create_user()`:

```c
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

    /*
     * Temporarily switch to the new address space so the kernel can write
     * through the process's user virtual addresses.
     *
     * Interrupts stay disabled during this short copy so the scheduler cannot
     * run while we are borrowing the new address space.
     */
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
```

Then update the test title and success message.

Replace:

```c
console_writeln("Process test: starting fd read/write user process test");
```

with:

```c
console_writeln("Process test: starting isolated address-space syscall test");
```

Replace:

```c
console_writeln("Process test: fd read/write/sleep/exit sanity check passed");
```

with:

```c
console_writeln("Process test: isolated address-space syscall sanity check passed");
```

---

# 13. Why temporarily switching address spaces is safe here

The process’s user page exists only in that process page directory.

So the kernel cannot write this directly while using the kernel address space:

```c
memcpy((void *)0x40100000, program, program_size);
```

because `0x40100000` is not mapped in the kernel address space anymore.

So we do:

```text
disable interrupts
save old address space
switch to process address space
copy program into user virtual address
clear user stack
switch back
restore interrupts
```

This is safe because all kernel code, kernel stacks, and kernel heap mappings are shared into both address spaces.

Later, we can avoid temporary CR3 switching by creating a temporary kernel mapping for physical pages, but this approach is easier to understand for now.

---

# 14. Update `kernel/kmain.c`

Add:

```c
#include "kernel/address_space.h"
```

Then call:

```c
address_space_init();
```

after `vmem_init()`.

The relevant section should become:

```c
vmem_init();
address_space_init();
vmem_test_once();

heap_init(4);
heap_test_once();

threading_init();
process_init_system();
thread_test_once();
```

The order matters.

`address_space_init()` must happen after paging and VMM are initialized, but before process creation.

---

# 15. Update `Makefile`

Add:

```text
build/kernel/address_space.o
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
    build/kernel/usercopy.o \
    build/kernel/usermode.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the process greps.

Replace:

```make
grep -q "Process test: starting fd read/write user process test" build/test.log
grep -q "Process test: fd read/write/sleep/exit sanity check passed" build/test.log
```

with:

```make
grep -q "Address space: kernel address space registered" build/test.log
grep -q "Address space: created process page directory" build/test.log
grep -q "Process test: starting isolated address-space syscall test" build/test.log
grep -q "Process test: isolated address-space syscall sanity check passed" build/test.log
```

The process-related test block should now include:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting isolated address-space syscall test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "Process: created pid=1 name=stdio-demo" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process stdio-demo pid=1 exited code 9" build/test.log
	grep -q "Process test: isolated address-space syscall sanity check passed" build/test.log
```

---

# 16. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 19 checks passed."
```

---

# 17. Expected output

A successful boot should include:

```text
Address space: kernel address space registered
...
Process: process table initialized
...
Process test: starting isolated address-space syscall test
Thread: created stdio-demo id=...
Address space: created process page directory
Process: created pid=1 name=stdio-demo
user>
toyix
echo: toyix
Syscall: process stdio-demo pid=1 exited code 9
Threads: reaping zombie stdio-demo id=...
Process test: isolated address-space syscall sanity check passed
```

That proves:

```text
new process page directory created
user code page mapped privately
user stack page mapped privately
scheduler switched CR3 for user process
syscalls worked from the process address space
copy_from_user checked current process mappings
copy_to_user wrote into current process mappings
```

This is a major architectural milestone.

---

# 18. Common failures

## Failure: page fault while copying program into `0x40100000`

Likely cause:

```text
you copied into user virtual memory before switching to the process address space
```

The copy must happen inside:

```c
address_space_switch(process->address_space);
memcpy((void *)process->user_code_base, program, program_size);
address_space_switch(old_space);
```

## Failure: kernel crashes after switching to process address space

Likely causes:

```text
kernel low identity mappings were not copied
kernel high mappings were not copied
kernel heap mappings are missing
current kernel stack is not mapped in the process address space
```

Check `sync_kernel_mappings()`.

For this intermediate low-kernel design, it must copy:

```text
PDEs for low 16 MiB
PDEs from 0xC0000000 upward
```

## Failure: `copy_from_user()` fails for valid user buffer

Likely causes:

```text
user page was not mapped ADDRESS_SPACE_FLAG_USER
usercopy is still checking old vmem_get_flags()
address_space_current() is not the process address space during syscall
scheduler did not call address_space_switch()
```

Check:

```c
address_space_switch(thread_address_space(next_thread));
```

inside `thread_schedule_from_interrupt()`.

## Failure: process thread runs before process pointer is attached

Use suspended creation:

```c
thread_t *thread = thread_create_suspended(...);
thread_set_process(thread, process);
thread_start(thread);
```

Do not use plain `thread_create()` for user processes now.

## Failure: all processes still see the same user memory

Likely cause:

```text
process_create_user() is still using vmem_map_page()
```

Process user pages must be mapped into:

```c
process->address_space
```

using:

```c
address_space_map_page()
```

not global kernel VMM mapping.

---

# 19. What this chapter achieved

Before this chapter:

```text
all user processes shared the same page directory
```

After this chapter:

```text
kernel has a kernel address space
each process has its own page directory
kernel mappings are shared into every process
user mappings are private to each process
scheduler switches CR3 when switching processes
```

The memory model is now:

```text
process A
  CR3 -> page directory A
  0x40100000 -> physical page A1

process B
  CR3 -> page directory B
  0x40100000 -> physical page B1

kernel
  shared mappings visible in both
```

That is the foundation for real process isolation.

---

# 20. Design limitations

This chapter is still not a final memory model.

Important limitations remain:

```text
kernel is still low identity-mapped
low 16 MiB is shared into every process as supervisor memory
no page-directory teardown yet
no freeing of process user pages yet
no demand paging
no copy-on-write
no user heap
no ELF loader
no guard pages
```

The next memory cleanup should eventually be:

```text
move kernel to higher half
remove most low identity mappings
map only needed trampoline/bootstrap pages low
make user/kernel split cleaner
```

But before doing that, we can build the executable loader path.

---

# 21. Next chapter

The next practical chapter should introduce a tiny executable format before full ELF.

A simple format could be:

```text
TOYEXE header
  magic
  entry offset
  code size
  data size
  bss size
```

Then the kernel can load a user program from an in-kernel byte array into a fresh process address space.

That gives us:

```text
loader abstraction
program image abstraction
separate code/data placement
entry point
user stack setup
```

After that, moving to ELF becomes much easier.

---

# 22. Resources

- [Chapter 19 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_19)
- [Chapter 19 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_19)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki: Paging](https://wiki.osdev.org/Paging)
- [OSDev Wiki: Context Switching](https://wiki.osdev.org/Context_Switching)

---

# 23. Closure

Toyix now gives each user process a private page directory while keeping the kernel mappings available across address spaces. The scheduler switches `CR3` as it switches threads, and the syscall copy path validates buffers against the process that is actually running.

Happy Coding!
