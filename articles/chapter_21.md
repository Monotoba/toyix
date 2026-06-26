# Chapter 21 — Process Teardown and Address-Space Cleanup

In Chapter 20, we added the TOYEXE loader:

```text
TOYEXE image
  ↓
loader validates header
  ↓
loader maps user image
  ↓
loader maps user stack
  ↓
loader starts process
```

But there is still a lifecycle problem.

The process can exit, but we are not yet reclaiming everything it owns.

Right now this leaks:

```text
process_t object
process page directory
process user page tables
process user code page
process user stack page
future BSS/data/user heap pages
```

This chapter closes that loop.

After this chapter, the process lifecycle becomes:

```text
create process
  ↓
load executable
  ↓
run user thread
  ↓
process exits
  ↓
wait collects exit status
  ↓
destroy process
  ↓
free user pages
  ↓
free user page tables
  ↓
free page directory
  ↓
free process object
```

The milestone output will look like:

```text
Process test: starting TOYEXE lifecycle cleanup test
TOYEXE: loaded image bytes=256 bss=64
Process: created pid=1 name=toyexe-demo
user> toyix
echo: toyix
Syscall: process toyexe-demo pid=1 exited code 9
Address space: destroyed process page directory, user pages=2 tables=2
Process: destroyed pid=1 name=toyexe-demo
Process test: TOYEXE lifecycle cleanup sanity check passed
```

---

# 1. What this chapter adds

We will add ownership tracking to `address_space_t`.

Each process address space will record every user page it maps:

```text
virtual page → physical page
```

Then `address_space_destroy()` can walk that list and free the physical pages.

We will also add:

```text
process_wait()
process_destroy()
```

That gives us a complete process lifecycle.

Modify:

```text
include/kernel/address_space.h
kernel/address_space.c
include/kernel/process.h
kernel/process.c
Makefile
tests/smoke.sh
```

No new assembly is needed.

---

# 2. Update `include/kernel/address_space.h`

Replace it with this version.

```c
// include/kernel/address_space.h
#ifndef TOYIX_KERNEL_ADDRESS_SPACE_H
#define TOYIX_KERNEL_ADDRESS_SPACE_H

#include <stdint.h>

#define ADDRESS_SPACE_FLAG_WRITABLE 0x00000001u
#define ADDRESS_SPACE_FLAG_USER     0x00000002u

#define ADDRESS_SPACE_USER_BASE 0x01000000u
#define ADDRESS_SPACE_USER_TOP  0xC0000000u

typedef struct address_mapping {
    uintptr_t virtual_addr;
    uintptr_t physical_addr;

    struct address_mapping *next;
} address_mapping_t;

typedef struct address_space {
    uint32_t magic;

    uint32_t *page_directory;
    uintptr_t page_directory_physical;

    address_mapping_t *user_mappings;

    uint32_t user_page_count;
} address_space_t;

void address_space_init(void);

address_space_t *address_space_kernel(void);
address_space_t *address_space_current(void);

address_space_t *address_space_create(void);
void address_space_destroy(address_space_t *space);

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

The key addition is:

```c
address_mapping_t *user_mappings;
```

That list is how the address space remembers which physical pages it owns.

---

# 3. Replace `kernel/address_space.c`

This version tracks owned user mappings and adds `address_space_destroy()`.

```c
// kernel/address_space.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/paging.h"
#include "kernel/address_space.h"
#include "kernel/console.h"
#include "kernel/heap.h"
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
     * These mappings remain supervisor-only because the source kernel PDE/PTE
     * entries do not have the user bit set.
     */
    uint32_t low_shared_entries =
        LOW_IDENTITY_SHARED_BYTES / (4u * 1024u * 1024u);

    for (uint32_t i = 0; i < low_shared_entries; ++i) {
        dir[i] = kernel_dir[i];
    }

    /*
     * Share the high kernel region.
     */
    uint32_t high_start = directory_index(KERNEL_HIGH_BASE);

    for (uint32_t i = high_start; i < PAGE_DIRECTORY_ENTRIES; ++i) {
        dir[i] = kernel_dir[i];
    }
}

static void mapping_add(
    address_space_t *space,
    uintptr_t virtual_addr,
    uintptr_t physical_addr
) {
    address_mapping_t *mapping =
        (address_mapping_t *)kmalloc(sizeof(address_mapping_t));

    if (mapping == 0) {
        kernel_panic("address_space: could not allocate mapping node");
    }

    mapping->virtual_addr = virtual_addr;
    mapping->physical_addr = physical_addr;
    mapping->next = space->user_mappings;

    space->user_mappings = mapping;
}

static int mapping_remove(
    address_space_t *space,
    uintptr_t virtual_addr
) {
    address_mapping_t *prev = 0;
    address_mapping_t *cur = space->user_mappings;

    while (cur != 0) {
        if (cur->virtual_addr == virtual_addr) {
            if (prev != 0) {
                prev->next = cur->next;
            } else {
                space->user_mappings = cur->next;
            }

            kfree(cur);
            return 1;
        }

        prev = cur;
        cur = cur->next;
    }

    return 0;
}

void address_space_init(void) {
    kernel_space.magic = ADDRESS_SPACE_MAGIC;
    kernel_space.page_directory = paging_get_kernel_directory();
    kernel_space.page_directory_physical =
        (uintptr_t)paging_get_kernel_directory();
    kernel_space.user_mappings = 0;
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
    space->user_mappings = 0;
    space->user_page_count = 0;

    sync_kernel_mappings(space);

    console_writeln("Address space: created process page directory");

    return space;
}

void address_space_switch(address_space_t *space) {
    validate_space(space);

    /*
     * Refresh shared kernel PDEs before every switch. This helps when the
     * kernel grows or adds mappings after a process address space was created.
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

    mapping_add(space, virtual_addr, physical_addr);

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

    uintptr_t physical = table[tab] & X86_PAGE_FRAME_MASK;

    table[tab] = 0;

    mapping_remove(space, virtual_addr);

    if (space->user_page_count > 0) {
        space->user_page_count--;
    }

    pmm_free_page(physical);

    if (space == current_space) {
        paging_invalidate_page(virtual_addr);
    }

    return 0;
}

static uint32_t free_user_page_tables(address_space_t *space) {
    uint32_t freed_tables = 0;

    uint32_t start_dir = directory_index(ADDRESS_SPACE_USER_BASE);
    uint32_t end_dir = directory_index(ADDRESS_SPACE_USER_TOP - 1u);

    for (uint32_t dir = start_dir; dir <= end_dir; ++dir) {
        uint32_t pde = space->page_directory[dir];

        if ((pde & X86_PAGE_PRESENT) == 0) {
            continue;
        }

        uintptr_t table_phys = pde & X86_PAGE_FRAME_MASK;

        space->page_directory[dir] = 0;

        pmm_free_page(table_phys);
        freed_tables++;
    }

    return freed_tables;
}

void address_space_destroy(address_space_t *space) {
    validate_space(space);

    if (space == &kernel_space) {
        kernel_panic("attempted to destroy kernel address space");
    }

    /*
     * Never destroy the active address space. Switch back to the kernel
     * directory first.
     */
    if (current_space == space) {
        address_space_switch(&kernel_space);
    }

    uint32_t freed_pages = 0;

    while (space->user_mappings != 0) {
        uintptr_t virtual_addr = space->user_mappings->virtual_addr;

        if (address_space_unmap_page(space, virtual_addr) != 0) {
            kernel_panic("address_space_destroy failed to unmap user page");
        }

        freed_pages++;
    }

    uint32_t freed_tables = free_user_page_tables(space);

    uintptr_t directory_phys = space->page_directory_physical;

    space->magic = 0;
    space->page_directory = 0;
    space->page_directory_physical = 0;
    space->user_mappings = 0;
    space->user_page_count = 0;

    pmm_free_page(directory_phys);
    kfree(space);

    console_write("Address space: destroyed process page directory, user pages=");
    console_write_u32_dec(freed_pages);
    console_write(" tables=");
    console_write_u32_dec(freed_tables);
    console_putc('\n');
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

# 4. Why we free page tables separately

The mapping list tracks user pages:

```text
0x40100000 → physical image page
0x6FFFF000 → physical stack page
```

But the page tables are separate physical pages.

For example, this process uses two user virtual regions:

```text
0x40100000    program image/BSS
0x6FFFF000    user stack
```

Those addresses live under different page-directory entries, so the process usually owns:

```text
2 user pages
2 user page tables
1 page directory
```

The mapping list frees the user pages.

`free_user_page_tables()` frees the page tables.

Then `address_space_destroy()` frees the page directory itself.

---

# 5. Update `include/kernel/process.h`

Replace it with this version.

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
    PROCESS_EXITED,
    PROCESS_DESTROYED
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

uint32_t process_wait(process_t *process);
void process_destroy(process_t *process);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

void process_test_once(void);

#endif
```

The new functions are:

```c
uint32_t process_wait(process_t *process);
void process_destroy(process_t *process);
```

---

# 6. Update `kernel/process.c`

Most of `process.c` stays from Chapter 20, but we add wait/destroy and update the test.

Make sure the includes include `kernel/heap.h`, because `process_destroy()` frees the process object:

```c
#include "kernel/heap.h"
```

Add this helper near the existing validation function:

```c
static void validate_live_process(process_t *process) {
    validate_process(process);

    if (process->state == PROCESS_DESTROYED) {
        kernel_panic("process: operation on destroyed process");
    }
}
```

Now add `process_wait()` and `process_destroy()` after `process_exit_current()`.

```c
uint32_t process_wait(process_t *process) {
    validate_live_process(process);

    /*
     * A process should not wait on itself.
     *
     * Later, waitpid-style parent/child relationships will handle this more
     * carefully. For now, process_wait() is a kernel-side collection helper.
     */
    if (process_current() == process) {
        kernel_panic("process attempted to wait on itself");
    }

    while (!process->exited) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    /*
     * Make sure the exited main thread has been collected before process
     * teardown frees the process object.
     */
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

    process->state = PROCESS_DESTROYED;
    process->magic = 0;

    kfree(process);

    console_write("Process: destroyed pid=");
    console_write_u32_dec(pid);
    console_write(" name=");
    console_writeln(name);
}
```

## Why `process_destroy()` does not free the thread

The thread is freed by:

```c
thread_reap_zombies();
```

A thread must not free its own stack while still running on it. That is why exit creates a zombie, and the reaper later frees the thread object and kernel stack.

`process_wait()` calls `thread_reap_zombies()` before destruction.

So the process teardown order is:

```text
process exits
  ↓
thread becomes zombie
  ↓
process_wait() observes exit
  ↓
thread_reap_zombies() frees thread and kernel stack
  ↓
process_destroy() frees address space and process object
```

---

# 7. Update `process_test_once()` in `kernel/process.c`

Replace the Chapter 20 version with this one.

```c
void process_test_once(void) {
    console_writeln("Process test: starting TOYEXE lifecycle cleanup test");

    last_exit_seen = 0;
    last_exit_code = 0xFFFFFFFFu;

    static uint8_t toyexe_image[
        sizeof(toyexe_header_t) + DEMO_IMAGE_SIZE
    ];

    uint32_t toyexe_size = build_stdio_demo_toyexe(
        toyexe_image,
        sizeof(toyexe_image)
    );

    process_t *process = toyexe_create_process(
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

    uint32_t exit_code = process_wait(process);

    if (exit_code != 9) {
        kernel_panic("TOYEXE process test received wrong exit code");
    }

    process_destroy(process);

    console_writeln("Process test: TOYEXE lifecycle cleanup sanity check passed");
}
```

This replaces the old global `last_exit_seen` loop with the new process-specific wait helper.

We still keep `last_exit_seen` and `last_exit_code` for now because they are useful smoke-test state and simple diagnostics, but the test now follows the better lifecycle:

```text
toyexe_create_process()
process_wait()
process_destroy()
```

---

# 8. What gets freed now?

For the current TOYEXE test process, teardown should free:

```text
1 user image/BSS physical page
1 user stack physical page
2 user page tables
1 process page directory
1 process_t object
1 user thread object
1 user thread kernel stack
```

The thread object and thread kernel stack are freed by:

```c
thread_reap_zombies();
```

The address-space objects are freed by:

```c
address_space_destroy();
```

The process object is freed by:

```c
kfree(process);
```

One important limitation remains: our heap does not yet return completely unused heap pages back to the PMM. So freeing a `process_t`, thread object, or mapping node returns memory to the kernel heap, but not necessarily to the physical page allocator.

That is expected with our current heap design.

---

# 9. Update `Makefile`

Update the process-related greps.

Replace:

```make
grep -q "Process test: starting TOYEXE user program test" build/test.log
grep -q "Process test: TOYEXE load/read/write/sleep/exit sanity check passed" build/test.log
```

with:

```make
grep -q "Process test: starting TOYEXE lifecycle cleanup test" build/test.log
grep -q "Address space: destroyed process page directory" build/test.log
grep -q "Process: destroyed pid=1 name=toyexe-demo" build/test.log
grep -q "Process test: TOYEXE lifecycle cleanup sanity check passed" build/test.log
```

The process block should now look like this:

```make
	grep -q "Address space: kernel address space registered" build/test.log
	grep -q "Process: process table initialized" build/test.log
	grep -q "Process test: starting TOYEXE lifecycle cleanup test" build/test.log
	grep -q "Address space: created process page directory" build/test.log
	grep -q "TOYEXE: loaded image bytes=256 bss=64" build/test.log
	grep -q "Process: created pid=1 name=toyexe-demo" build/test.log
	grep -q "echo: toyix" build/test.log
	grep -q "Syscall: process toyexe-demo pid=1 exited code 9" build/test.log
	grep -q "Address space: destroyed process page directory" build/test.log
	grep -q "Process: destroyed pid=1 name=toyexe-demo" build/test.log
	grep -q "Process test: TOYEXE lifecycle cleanup sanity check passed" build/test.log
```

Update the final success line:

```make
	@echo "Boot, memory, heap, sync, monitor, TOYEXE, and process cleanup smoke test passed."
```

---

# 10. Update `tests/smoke.sh`

No structural change is needed.

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 21 checks passed."
```

---

# 11. Expected output

A successful boot should include:

```text
Process test: starting TOYEXE lifecycle cleanup test
Address space: created process page directory
TOYEXE: loaded image bytes=256 bss=64 entry=0x40100000
Thread: created toyexe-demo id=...
Process: created pid=1 name=toyexe-demo
user> toyix
echo: toyix
Syscall: process toyexe-demo pid=1 exited code 9
Threads: reaping zombie toyexe-demo id=...
Address space: destroyed process page directory, user pages=2 tables=2
Process: destroyed pid=1 name=toyexe-demo
Process test: TOYEXE lifecycle cleanup sanity check passed
```

The exact thread ID may vary.

The important new lines are:

```text
Address space: destroyed process page directory, user pages=2 tables=2
Process: destroyed pid=1 name=toyexe-demo
```

That proves teardown is now happening.

---

# 12. Common failures

## Failure: page fault during `address_space_destroy()`

Likely causes:

```text
destroying the currently active address space
kernel mappings missing from kernel address space
destroying page tables before user pages are unmapped
```

Check this block:

```c
if (current_space == space) {
    address_space_switch(&kernel_space);
}
```

The kernel should switch away before freeing the process page directory.

## Failure: process exits but destroy panics

Likely cause:

```text
process_destroy() called before process_wait()
```

Correct order:

```c
uint32_t exit_code = process_wait(process);
process_destroy(process);
```

Do not destroy a running process yet. Forced termination can be added later.

## Failure: user pages count is zero during destroy

Likely cause:

```text
address_space_map_page() did not call mapping_add()
```

Check:

```c
mapping_add(space, virtual_addr, physical_addr);
space->user_page_count++;
```

Without the mapping list, `address_space_destroy()` has no way to know which user physical pages to free.

## Failure: page tables are freed but user pages leak

Likely cause:

```text
address_space_destroy() frees page tables directly without first unmapping pages
```

Correct order:

```text
1. unmap/free user pages
2. free user page tables
3. free page directory
```

## Failure: crash after process destroy when scheduler runs

Likely cause:

```text
destroyed process while its thread was still ready/running/zombie
```

Make sure `process_wait()` reaps zombies before destroy:

```c
thread_reap_zombies();
```

The current design assumes the process has exited and its main thread has been reaped before process destruction.

---

# 13. What this chapter achieved

Before this chapter:

```text
process exits
  ↓
thread zombie eventually reaped
  ↓
process address space leaks
  ↓
user pages leak
  ↓
page tables leak
```

After this chapter:

```text
process exits
  ↓
process_wait() collects exit status
  ↓
thread zombie reaped
  ↓
process_destroy() frees address space
  ↓
user pages freed
  ↓
user page tables freed
  ↓
page directory freed
  ↓
process object freed
```

We now have a full basic process lifecycle.

---

# 14. Design limitations

This is still a minimal teardown model.

Missing pieces:

```text
parent/child process relationships
waitpid(pid)
exit status table
forced process kill
process list
reference counting
multi-threaded process teardown
file descriptor cleanup
current working directory cleanup
signal/event cleanup
address-space teardown for partially failed loads
```

The current model is still good enough for our next loader work.

The important thing is that a normal process can now be created, run, exited, waited, destroyed, and cleaned up.

---

# 15. Commit this chapter

After tests pass:

```bash
git status
git add .
git commit -m "Add process teardown and address-space cleanup"
```

This is an important stability checkpoint. We can now load and destroy user processes without permanently leaking their user memory.

---

# 16. Next chapter

Now that normal teardown exists, the next useful chapter is to improve process waiting and exit-status collection.

The current `process_wait()` helper is enough for a kernel-driven smoke test, but it is still too narrow for a fuller process model.

The next milestone should move toward:

```text
stable exit status collection
process lookup beyond the current thread
wait helpers that prepare for parent/child relationships
clearer ownership around process lifetime
```

That will make the process layer sturdy enough for a process table, `wait`-style syscalls, and later loader work.

---

# 17. Resources

- [Chapter 21 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_21)
- [Chapter 21 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_21)
- [Intel 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki: Paging](https://wiki.osdev.org/Paging)
- [OSDev Wiki: Processes and Threads](https://wiki.osdev.org/Processes_and_Threads)

---

# 18. Closure

Chapter 21 closes the first basic process lifecycle in Toyix. A TOYEXE user program can now be created, run, waited on, destroyed, and cleaned up without leaking its user pages or page tables.

Happy Coding!
