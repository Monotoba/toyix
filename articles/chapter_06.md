# Chapter 6 — Building the First Kernel Heap

In Chapter 5, we enabled paging with a simple identity map:

```text
virtual 0x00100000 → physical 0x00100000
virtual 0x00200000 → physical 0x00200000
virtual 0x00B8000 → physical 0x00B8000
```

That gave us a working paged kernel, but not yet a comfortable way to allocate variable-sized objects.

The physical memory manager gives us whole pages:

```c
uintptr_t page = pmm_alloc_page();
```

But kernel code usually needs smaller objects:

```c
struct task *t = kmalloc(sizeof(struct task));
char *buffer = kmalloc(128);
struct vnode *node = kmalloc(sizeof(struct vnode));
```

So this chapter builds the first **kernel heap**.

We will keep it honest: this is not the final allocator. It is an **early kernel heap** backed by identity-mapped physical pages below 16 MiB. That limitation exists because our current paging setup only identity-maps the first 16 MiB. If the PMM handed us a page at, say, physical `0x04000000`, the CPU would page fault when we tried to use it as a pointer.

So this chapter adds:

```text
PMM page allocation below a physical limit
early heap page growth
free-list allocator
block splitting
block coalescing
kmalloc()
kcalloc()
kfree()
heap statistics
heap sanity test
```

We will wrap the allocator behind a small interface so later we can replace it with a better allocator without rewriting the rest of the kernel.

GCC’s freestanding mode does not give our kernel a hosted C library; GCC also documents that freestanding environments are expected to provide low-level routines such as `memcpy`, `memmove`, `memset`, and `memcmp`, which is why we already added our own memory functions earlier. ([GCC][1])

---

## Housekeeping

We need to do a little housekeeping before moving forward.

We have cluttered our `kernel/` folder with header files that really should be
placed in `include/kernel/`. From here forward, kernel-specific public headers
will live under `include/kernel/`. That keeps implementation files in
`kernel/` and shared kernel interfaces in `include/kernel/`.

So, before moving on to the heap implementation, move:

```text
kernel/idle.h -> include/kernel/idle.h
kernel/pmm.h  -> include/kernel/pmm.h
```

Then update the include statements in:

```text
kernel/idle.c
kernel/pmm.c
kernel/kmain.c
```

Those source files should include the moved headers through the existing
kernel include path:

```c
#include "kernel/idle.h"
#include "kernel/pmm.h"
```

This works because the Makefile already passes `-Iinclude`, so
`include/kernel/pmm.h` is visible to the compiler as `kernel/pmm.h`.

---

# 1. The allocator design

Our first heap will use a classic block-header free list.

Each heap block looks like this in memory:

```text
+--------------------+
| heap_block_t       |
+--------------------+
| user payload       |
| returned by kmalloc|
+--------------------+
```

A free block and an allocated block both have a header.

The header stores:

```text
magic number
payload size
free/used flag
next block pointer
previous block pointer
```

On 32-bit x86, that structure is 20 bytes. Because this heap promises
8-byte-aligned payloads, the allocator rounds the header span up before placing
user data after it. That is why the implementation uses `heap_header_size()`
instead of raw `sizeof(heap_block_t)` when splitting, coalescing, returning
payload pointers, and converting a payload pointer back to its block header.

When `kmalloc(size)` runs:

```text
1. Round size up to 8-byte alignment.
2. Search the free list for a large enough free block.
3. If none exists, grow the heap by asking the PMM for more pages.
4. Split the free block if it is much larger than needed.
5. Mark the selected block used.
6. Return the address immediately after the header.
```

When `kfree(ptr)` runs:

```text
1. Find the block header immediately before ptr.
2. Validate the magic number.
3. Mark the block free.
4. Coalesce with adjacent free blocks when possible.
```

This gives us a simple reusable kernel allocator.

---

# 2. Important limitation

For now, the heap can only grow using physical pages below:

```c
0x01000000
```

That is 16 MiB.

Why?

Because Chapter 5 identity-mapped only the first 16 MiB. Intel’s IA-32 manuals describe paging as address translation through page structures controlled by CPU registers such as CR0 and CR3; after paging is enabled, memory accesses use virtual-to-physical translation rather than raw physical addressing. ([Intel][2])

So if we allocate a physical page that is not currently mapped, we cannot safely treat that number as a C pointer.

Later, we will add a true virtual memory manager:

```text
vmm_map_page(virtual, physical, flags)
```

Then the heap can grow into a dedicated virtual heap region, such as:

```text
0xD0000000 - 0xDFFFFFFF
```

But today, identity-mapped low physical pages are enough to get a working heap.

---

# 3. Patch overview

Add:

```text
include/kernel/
├── heap.h
└── string.h

kernel/
└── heap.c
```

Modify:

```text
include/kernel/idle.h
include/kernel/pmm.h
kernel/pmm.c
kernel/lib/mem.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

---

# 4. Add `include/kernel/string.h`

```c
// include/kernel/string.h
#ifndef TOYIX_KERNEL_STRING_H
#define TOYIX_KERNEL_STRING_H

#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *left, const void *right, size_t count);

#endif
```

## Why add this header?

We already wrote `memset`, `memcpy`, `memmove`, and `memcmp` in Chapter 1.

But now more kernel code will call them directly.

Rather than writing local `extern` declarations, we give the kernel a small internal string/memory header.

This is still not a full libc. It is just the beginning of our own freestanding kernel support library.

---

# 5. Update `kernel/lib/mem.c`

Replace the top of `kernel/lib/mem.c` so it includes the new header.

```c
// kernel/lib/mem.c
#include <stddef.h>
#include "kernel/string.h"

void *memset(void *dest, int value, size_t count) {
    unsigned char *d = (unsigned char *)dest;

    for (size_t i = 0; i < count; ++i) {
        d[i] = (unsigned char)value;
    }

    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || count == 0) {
        return dest;
    }

    if (d < s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }

    return dest;
}

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;

    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}
```

---

# 6. Update `include/kernel/pmm.h`

We need a way to ask the PMM for pages below the current identity-mapped limit.

Replace `include/kernel/pmm.h` with this version.

```c
// include/kernel/pmm.h
#ifndef TOYIX_KERNEL_PMM_H
#define TOYIX_KERNEL_PMM_H

#include <stdint.h>
#include "arch/x86/multiboot.h"

#define PMM_PAGE_SIZE 4096u
#define PMM_INVALID_PAGE 0u

typedef struct pmm_stats {
    uint32_t total_frames;
    uint32_t usable_frames;
    uint32_t reserved_frames;
    uint32_t free_frames;
    uint32_t used_frames;
    uint32_t highest_physical_addr;
} pmm_stats_t;

void pmm_init(const multiboot_info_t *mbi);

uintptr_t pmm_alloc_page(void);
uintptr_t pmm_alloc_page_below(uintptr_t max_exclusive);
uintptr_t pmm_alloc_contiguous_pages_below(
    uint32_t page_count,
    uintptr_t max_exclusive
);

void pmm_free_page(uintptr_t physical_addr);

pmm_stats_t pmm_get_stats(void);
void pmm_dump_stats(void);
void pmm_test_once(void);

#endif
```

## Why add “below limit” allocation?

Our current heap needs pages that are already mapped.

Since Chapter 5 mapped the first 16 MiB, the heap will ask for pages below:

```c
0x01000000
```

This is not a permanent design. It is a safe bridge between:

```text
physical page allocator
```

and the later:

```text
virtual memory manager
```

---

# 7. Update `kernel/pmm.c`

Add these functions after `pmm_alloc_page()` in `kernel/pmm.c`.

```c
uintptr_t pmm_alloc_page_below(uintptr_t max_exclusive) {
    uint32_t max_frame = (uint32_t)(max_exclusive / PMM_PAGE_SIZE);

    if (max_frame > PMM_MAX_FRAMES) {
        max_frame = PMM_MAX_FRAMES;
    }

    for (uint32_t frame = 256; frame < max_frame; ++frame) {
        if (!bitmap_test(frame)) {
            mark_frame_used(frame);
            return addr_from_frame_index(frame);
        }
    }

    return PMM_INVALID_PAGE;
}

uintptr_t pmm_alloc_contiguous_pages_below(
    uint32_t page_count,
    uintptr_t max_exclusive
) {
    if (page_count == 0) {
        return PMM_INVALID_PAGE;
    }

    uint32_t max_frame = (uint32_t)(max_exclusive / PMM_PAGE_SIZE);

    if (max_frame > PMM_MAX_FRAMES) {
        max_frame = PMM_MAX_FRAMES;
    }

    uint32_t run_start = 0;
    uint32_t run_length = 0;

    for (uint32_t frame = 256; frame < max_frame; ++frame) {
        if (!bitmap_test(frame)) {
            if (run_length == 0) {
                run_start = frame;
            }

            run_length++;

            if (run_length == page_count) {
                for (uint32_t i = 0; i < page_count; ++i) {
                    mark_frame_used(run_start + i);
                }

                return addr_from_frame_index(run_start);
            }
        } else {
            run_start = 0;
            run_length = 0;
        }
    }

    return PMM_INVALID_PAGE;
}
```

## Why contiguous pages?

The first heap is identity-mapped.

If the heap needs to grow by three pages, it needs those pages to appear as one continuous region in the current virtual address space.

Without a real `vmm_map_page()` function, we cannot stitch arbitrary physical pages together into one virtual heap range.

So for now, heap growth requests contiguous physical pages below 16 MiB.

Later, once we have real virtual mapping, the heap will no longer need physically contiguous pages. It will ask for any physical pages and map them into contiguous virtual heap space.

That will be a major improvement.

---

# 8. Add `include/kernel/heap.h`

```c
// include/kernel/heap.h
#ifndef TOYIX_KERNEL_HEAP_H
#define TOYIX_KERNEL_HEAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct heap_stats {
    uint32_t region_bytes;
    uint32_t free_payload_bytes;
    uint32_t used_payload_bytes;
    uint32_t block_count;
    uint32_t allocation_count;
} heap_stats_t;

void heap_init(uint32_t initial_pages);

void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void kfree(void *ptr);

heap_stats_t heap_get_stats(void);
void heap_dump_stats(void);
void heap_test_once(void);

#endif
```

## Why not call these `malloc` and `free`?

Kernel allocators are usually named differently from libc allocators.

We use:

```c
kmalloc()
kfree()
```

because they allocate from the kernel heap, not from a user process heap.

Later, user programs may have their own `malloc()` implemented in user-space libc, backed by syscalls such as:

```text
brk
mmap
```

The kernel’s own allocator should stay distinct.

---

# 9. Add `kernel/heap.c`

```c
// kernel/heap.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"

#define HEAP_MAGIC 0x48454150u
#define HEAP_ALIGNMENT 8u
#define HEAP_MIN_SPLIT_PAYLOAD 16u

/*
 * Chapter 5 identity-mapped the first 16 MiB.
 *
 * Until we implement real dynamic virtual memory mapping, the heap must only
 * use physical pages below this limit.
 */
#define HEAP_IDENTITY_LIMIT 0x01000000u

typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint32_t is_free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static heap_block_t *heap_head;
static int heap_initialized;
static uint32_t heap_region_bytes;
static uint32_t heap_allocation_count;

static size_t align_up_size(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t heap_header_size(void) {
    return (uint32_t)align_up_size(sizeof(heap_block_t), HEAP_ALIGNMENT);
}

static uintptr_t block_start_addr(const heap_block_t *block) {
    return (uintptr_t)block;
}

static uintptr_t block_end_addr(const heap_block_t *block) {
    return (uintptr_t)block + heap_header_size() + block->size;
}

static int blocks_are_adjacent(
    const heap_block_t *left,
    const heap_block_t *right
) {
    return block_end_addr(left) == block_start_addr(right);
}

static void validate_block(const heap_block_t *block) {
    if (block == 0) {
        kernel_panic("heap encountered null block");
    }

    if (block->magic != HEAP_MAGIC) {
        kernel_panic("heap block magic mismatch");
    }
}

static heap_stats_t heap_compute_stats(void) {
    heap_stats_t stats;

    stats.region_bytes = heap_region_bytes;
    stats.free_payload_bytes = 0;
    stats.used_payload_bytes = 0;
    stats.block_count = 0;
    stats.allocation_count = heap_allocation_count;

    for (heap_block_t *block = heap_head; block != 0; block = block->next) {
        validate_block(block);

        stats.block_count++;

        if (block->is_free) {
            stats.free_payload_bytes += block->size;
        } else {
            stats.used_payload_bytes += block->size;
        }
    }

    return stats;
}

static void insert_block_sorted(heap_block_t *new_block) {
    validate_block(new_block);

    if (heap_head == 0) {
        heap_head = new_block;
        new_block->prev = 0;
        new_block->next = 0;
        return;
    }

    heap_block_t *current = heap_head;

    while (current != 0 && current < new_block) {
        current = current->next;
    }

    if (current == heap_head) {
        new_block->prev = 0;
        new_block->next = heap_head;
        heap_head->prev = new_block;
        heap_head = new_block;
        return;
    }

    if (current == 0) {
        heap_block_t *tail = heap_head;

        while (tail->next != 0) {
            tail = tail->next;
        }

        tail->next = new_block;
        new_block->prev = tail;
        new_block->next = 0;
        return;
    }

    heap_block_t *previous = current->prev;

    previous->next = new_block;
    new_block->prev = previous;
    new_block->next = current;
    current->prev = new_block;
}

static heap_block_t *coalesce_with_next(heap_block_t *block) {
    validate_block(block);

    heap_block_t *next = block->next;

    if (next == 0) {
        return block;
    }

    validate_block(next);

    if (!block->is_free || !next->is_free) {
        return block;
    }

    if (!blocks_are_adjacent(block, next)) {
        return block;
    }

    block->size += heap_header_size() + next->size;
    block->next = next->next;

    if (next->next != 0) {
        next->next->prev = block;
    }

    return block;
}

static heap_block_t *coalesce_around(heap_block_t *block) {
    validate_block(block);

    block = coalesce_with_next(block);

    if (block->prev != 0) {
        heap_block_t *prev = block->prev;
        validate_block(prev);

        if (prev->is_free && blocks_are_adjacent(prev, block)) {
            block = coalesce_with_next(prev);
        }
    }

    return block;
}

static void split_block_if_large_enough(heap_block_t *block, uint32_t wanted) {
    validate_block(block);

    if (block->size < wanted) {
        kernel_panic("heap split requested block smaller than allocation");
    }

    uint32_t leftover = block->size - wanted;

    if (leftover < heap_header_size() + HEAP_MIN_SPLIT_PAYLOAD) {
        return;
    }

    heap_block_t *split = (heap_block_t *)(
        (uintptr_t)block + heap_header_size() + wanted
    );

    split->magic = HEAP_MAGIC;
    split->size = leftover - heap_header_size();
    split->is_free = 1;
    split->prev = block;
    split->next = block->next;

    if (block->next != 0) {
        block->next->prev = split;
    }

    block->next = split;
    block->size = wanted;
}

static heap_block_t *find_free_block(uint32_t wanted) {
    for (heap_block_t *block = heap_head; block != 0; block = block->next) {
        validate_block(block);

        if (block->is_free && block->size >= wanted) {
            return block;
        }
    }

    return 0;
}

static heap_block_t *heap_grow(uint32_t minimum_payload_bytes) {
    uint32_t needed_bytes =
        minimum_payload_bytes + heap_header_size();

    uint32_t pages =
        (needed_bytes + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;

    if (pages == 0) {
        pages = 1;
    }

    uintptr_t region = pmm_alloc_contiguous_pages_below(
        pages,
        HEAP_IDENTITY_LIMIT
    );

    if (region == PMM_INVALID_PAGE) {
        kernel_panic("heap could not grow below identity-mapped limit");
    }

    uint32_t region_bytes = pages * PMM_PAGE_SIZE;

    heap_block_t *block = (heap_block_t *)region;

    block->magic = HEAP_MAGIC;
    block->size = region_bytes - heap_header_size();
    block->is_free = 1;
    block->next = 0;
    block->prev = 0;

    heap_region_bytes += region_bytes;

    insert_block_sorted(block);
    coalesce_around(block);

    return block;
}

void heap_init(uint32_t initial_pages) {
    if (heap_initialized) {
        kernel_panic("heap_init called twice");
    }

    if (initial_pages == 0) {
        initial_pages = 1;
    }

    heap_head = 0;
    heap_region_bytes = 0;
    heap_allocation_count = 0;

    heap_initialized = 1;

    heap_grow((initial_pages * PMM_PAGE_SIZE) - heap_header_size());

    console_write("Heap: initialized with ");
    console_write_u32_dec(initial_pages);
    console_writeln(" page(s)");

    heap_dump_stats();
}

void *kmalloc(size_t size) {
    if (!heap_initialized) {
        kernel_panic("kmalloc called before heap_init");
    }

    if (size == 0) {
        return 0;
    }

    size_t aligned_size = align_up_size(size, HEAP_ALIGNMENT);

    if (aligned_size > 0xFFFFFFFFu) {
        kernel_panic("kmalloc request too large");
    }

    uint32_t wanted = (uint32_t)aligned_size;

    heap_block_t *block = find_free_block(wanted);

    if (block == 0) {
        heap_grow(wanted);
        block = find_free_block(wanted);
    }

    if (block == 0) {
        kernel_panic("kmalloc failed after heap growth");
    }

    split_block_if_large_enough(block, wanted);

    block->is_free = 0;
    heap_allocation_count++;

    return (void *)((uintptr_t)block + heap_header_size());
}

void *kcalloc(size_t count, size_t size) {
    if (count != 0 && size > ((size_t)-1) / count) {
        kernel_panic("kcalloc size overflow");
    }

    size_t total = count * size;

    void *ptr = kmalloc(total);

    if (ptr != 0) {
        memset(ptr, 0, total);
    }

    return ptr;
}

void kfree(void *ptr) {
    if (ptr == 0) {
        return;
    }

    heap_block_t *block = (heap_block_t *)(
        (uintptr_t)ptr - heap_header_size()
    );

    validate_block(block);

    if (block->is_free) {
        kernel_panic("double free detected");
    }

    block->is_free = 1;
    coalesce_around(block);
}

heap_stats_t heap_get_stats(void) {
    return heap_compute_stats();
}

void heap_dump_stats(void) {
    heap_stats_t stats = heap_get_stats();

    console_write("Heap: region bytes ");
    console_write_u32_dec(stats.region_bytes);
    console_putc('\n');

    console_write("Heap: free payload bytes ");
    console_write_u32_dec(stats.free_payload_bytes);
    console_putc('\n');

    console_write("Heap: used payload bytes ");
    console_write_u32_dec(stats.used_payload_bytes);
    console_putc('\n');

    console_write("Heap: block count ");
    console_write_u32_dec(stats.block_count);
    console_putc('\n');

    console_write("Heap: allocation count ");
    console_write_u32_dec(stats.allocation_count);
    console_putc('\n');
}

void heap_test_once(void) {
    console_writeln("Heap test: starting");

    char *a = (char *)kmalloc(32);
    char *b = (char *)kmalloc(100);
    uint32_t *c = (uint32_t *)kmalloc(3000);
    uint8_t *z = (uint8_t *)kcalloc(16, 4);

    if (a == 0 || b == 0 || c == 0 || z == 0) {
        kernel_panic("heap test allocation returned null");
    }

    for (uint32_t i = 0; i < 32; ++i) {
        a[i] = (char)('A' + (i % 26));
    }

    for (uint32_t i = 0; i < 100; ++i) {
        b[i] = (char)(i & 0x7F);
    }

    for (uint32_t i = 0; i < 3000 / sizeof(uint32_t); ++i) {
        c[i] = 0xA5A50000u + i;
    }

    for (uint32_t i = 0; i < 64; ++i) {
        if (z[i] != 0) {
            kernel_panic("heap test kcalloc did not zero memory");
        }
    }

    if ((((uintptr_t)a) & (HEAP_ALIGNMENT - 1u)) != 0 ||
        (((uintptr_t)b) & (HEAP_ALIGNMENT - 1u)) != 0 ||
        (((uintptr_t)c) & (HEAP_ALIGNMENT - 1u)) != 0 ||
        (((uintptr_t)z) & (HEAP_ALIGNMENT - 1u)) != 0) {
        kernel_panic("heap test returned unaligned allocation");
    }

    kfree(b);
    kfree(a);
    kfree(z);
    kfree(c);

    console_writeln("Heap test: allocation/free sanity check passed");
    heap_dump_stats();
}
```

---

# 10. Why this heap coalesces blocks

Suppose we allocate three blocks:

```text
[A used][B used][C used]
```

Then free `B`:

```text
[A used][B free][C used]
```

That gives us one reusable block.

But if we later free `A`, we do not want:

```text
[A free][B free][C used]
```

to stay split forever.

We want:

```text
[ A+B free ][C used]
```

That process is called **coalescing**.

Without coalescing, the heap fragments quickly. You might have plenty of total free memory but no single block large enough for the next allocation.

Our allocator coalesces only when two blocks are physically adjacent:

```c
block_end_addr(left) == block_start_addr(right)
```

That matters because separate heap growth regions might not be contiguous.

---

# 11. Why this heap splits blocks

Suppose the heap has one free 4096-byte page minus header overhead, and someone asks for 32 bytes.

Without splitting, the allocator would waste almost the whole page.

So `kmalloc(32)` splits a large free block:

```text
before:
[ big free block ]

after:
[ 32-byte used block ][ remaining free block ]
```

The condition:

```c
leftover >= heap_header_size() + HEAP_MIN_SPLIT_PAYLOAD
```

prevents creating tiny useless fragments while preserving the heap's payload
alignment guarantee.

---

# 12. Update `kernel/kmain.c`

Add the heap include:

```c
#include "kernel/heap.h"
```

Then initialize and test the heap after paging is enabled.

Replace the middle of `kernel_main()` with this updated sequence:

```c
gdt_init();
idt_init();
pic_init();

pmm_init(mbi);
pmm_test_once();

paging_init();
paging_test_identity_mapping();

heap_init(4);
heap_test_once();

#ifdef TOYIX_TRIGGER_PAGE_FAULT
    console_writeln("Triggering test page fault at 0xC0000000...");
    volatile uint32_t *bad = (volatile uint32_t *)0xC0000000u;
    uint32_t value = *bad;
    (void)value;
#endif

pit_init(100);
keyboard_init();
```

Here is the full updated file for clarity.

```c
// kernel/kmain.c
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/multiboot.h"
#include "arch/x86/paging.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "drivers/input/keyboard.h"
#include "kernel/idle.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"

extern const console_driver_t serial_console_driver;
extern const console_driver_t vga_text_console_driver;

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    console_register(&serial_console_driver);
    console_register(&vga_text_console_driver);
    console_init_all();

    console_writeln("Toyix kernel alive");

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        console_writeln("Boot protocol: Multiboot OK");
    } else {
        console_write("Boot protocol: unexpected magic ");
        console_write_hex32(multiboot_magic);
        console_putc('\n');
        kernel_panic("unsupported boot protocol");
    }

    const multiboot_info_t *mbi =
        (const multiboot_info_t *)(uintptr_t)multiboot_info_addr;

    console_write("Multiboot info at ");
    console_write_hex32(multiboot_info_addr);
    console_putc('\n');

    gdt_init();
    idt_init();
    pic_init();

    pmm_init(mbi);
    pmm_test_once();

    paging_init();
    paging_test_identity_mapping();

    heap_init(4);
    heap_test_once();

#ifdef TOYIX_TRIGGER_PAGE_FAULT
    console_writeln("Triggering test page fault at 0xC0000000...");
    volatile uint32_t *bad = (volatile uint32_t *)0xC0000000u;
    uint32_t value = *bad;
    (void)value;
#endif

    pit_init(100);
    keyboard_init();

    console_writeln("Interrupt hardware: configured");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    interrupts_enable();

    console_writeln("Interrupts: enabled");

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: dynamic virtual memory mapping and heap expansion above 16 MiB.");

    kernel_idle();
}
```

---

# 13. Update `Makefile`

Add:

```text
build/kernel/heap.o
```

to the object list.

Also keep the test-only build flags from poisoning later normal builds. The
exception and page-fault tests compile the kernel with `CFLAGS_EXTRA`; if a
normal `make test` reuses those object files, the kernel can still contain a
test-only fault trigger. Track `CFLAGS_EXTRA` in a small build stamp and make C
objects depend on it:

```make
.PHONY: all clean iso run test test-exception test-page-fault FORCE

build/.cflags: FORCE
	@mkdir -p build
	@if [[ ! -f $@ ]] || [[ "$$(cat $@)" != "$(CFLAGS_EXTRA)" ]]; then \
		printf '%s\n' "$(CFLAGS_EXTRA)" > $@; \
	fi
```

Then make C object rules depend on `build/.cflags`:

```make
build/arch/x86/paging.o: arch/x86/paging.c build/.cflags
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.c build/.cflags
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
```

Here is the relevant updated `OBJS` section:

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
    build/kernel/kmain.o \
    build/kernel/idle.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Also update the `test` target greps:

```make
	grep -q "Heap: initialized with 4 page(s)" build/test.log
	grep -q "Heap test: allocation/free sanity check passed" build/test.log
```

The updated test block should contain at least:

```make
test: iso
	@mkdir -p build
	@rm -f build/test.log
	@timeout 5s $(QEMU) \
		-boot d \
		-cdrom build/toyix.iso \
		-display none \
		-monitor none \
		-serial file:build/test.log \
		-no-reboot \
		2>/dev/null || true
	grep -q "Toyix kernel alive" build/test.log
	grep -q "Boot protocol: Multiboot OK" build/test.log
	grep -q "PMM: parsing Multiboot memory map" build/test.log
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
	grep -q "Paging test: identity-mapped kernel data is readable/writable" build/test.log
	grep -q "Heap: initialized with 4 page(s)" build/test.log
	grep -q "Heap test: allocation/free sanity check passed" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, IRQ, PMM, paging, and heap smoke test passed."
```

---

# 14. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception

echo "All smoke checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 15. Expected output

A normal boot should now include:

```text
Paging: enabled with identity map of first 16 MiB
Paging test: identity-mapped kernel data is readable/writable
Heap: initialized with 4 page(s)
Heap: region bytes 16384
Heap test: starting
Heap test: allocation/free sanity check passed
Interrupts: enabled
Timer: observed 3 ticks
```

The exact heap free/used numbers may vary slightly depending on structure size and alignment.

The important lines are:

```text
Heap: initialized with 4 page(s)
Heap test: allocation/free sanity check passed
```

---

# 16. Common failures

## Failure: page fault during `heap_init`

Likely cause:

```text
heap allocated a physical page outside the identity-mapped region
```

Check that `heap_grow()` uses:

```c
pmm_alloc_contiguous_pages_below(pages, HEAP_IDENTITY_LIMIT)
```

and that:

```c
#define HEAP_IDENTITY_LIMIT 0x01000000u
```

matches the identity-map size from Chapter 5.

## Failure: `heap block magic mismatch`

Likely causes:

```text
buffer overrun
freeing a pointer not returned by kmalloc
double free corrupted block list
bad split math
bad coalesce math
```

Check this pointer calculation:

```c
return (void *)((uintptr_t)block + heap_header_size());
```

and the inverse in `kfree()`:

```c
heap_block_t *block =
    (heap_block_t *)((uintptr_t)ptr - heap_header_size());
```

Those two must match exactly.

If the heap test reports unaligned allocations, check that all block-size math
uses `heap_header_size()` rather than raw `sizeof(heap_block_t)`. On i386 the
header structure is 20 bytes, but the payload needs to start on an 8-byte
boundary.

## Failure: `double free detected`

That means the same pointer was passed to `kfree()` twice.

That is a good panic. Silent double frees corrupt allocator state and become much harder to debug later.

## Failure: heap cannot grow below identity-mapped limit

Likely causes:

```text
not enough free RAM below 16 MiB
kernel image is too large
PMM reserved too much
PMM bitmap logic is wrong
```

For the tutorial kernel, this should not normally happen in QEMU.

---

# 17. What this chapter gives us

We now have this memory stack:

```text
Multiboot memory map
  ↓
physical page allocator
  ↓
identity paging
  ↓
early kernel heap
  ↓
kmalloc / kcalloc / kfree
```

That is a major step.

Now kernel subsystems can allocate dynamic objects.

For example, future chapters can allocate:

```text
keyboard input buffers
terminal line buffers
task structures
scheduler queues
VFS nodes
open file tables
ELF loader state
module descriptors
```

without each subsystem needing to manage raw pages.

---

# 18. Why this allocator is swappable

Right now, kernel code will call:

```c
void *p = kmalloc(size);
kfree(p);
```

Later, we can keep those public names but change the implementation underneath.

Possible future allocators:

| Allocator           | Use                                 |
| ------------------- | ----------------------------------- |
| bump allocator      | earliest boot only                  |
| free-list allocator | simple general heap                 |
| slab allocator      | many fixed-size kernel objects      |
| buddy allocator     | power-of-two page/block allocations |
| per-CPU allocator   | SMP scalability                     |
| debug allocator     | red zones, poisoning, leak tracking |

A common mature-kernel pattern is to use more than one allocator:

```text
PMM / buddy allocator      → physical pages
VMM                        → virtual mappings
kmalloc general heap       → variable-sized allocations
slab caches                → task structs, vnodes, inodes, file objects
```

Our current heap is only the first general-purpose layer.

---

# 19. Commit this chapter

After the tests pass:

```bash
git status
git add .
git commit -m "Add early kernel heap with kmalloc and kfree"
```

This is a good checkpoint before adding dynamic virtual memory mapping.

---

# 20. Next chapter

The natural next chapter is **dynamic virtual memory mapping**.

We need to move beyond:

```text
heap can only use pages below 16 MiB
```

and toward:

```text
heap owns a virtual range
physical pages can come from anywhere
VMM maps them into heap space
```

That means adding:

```c
vmm_map_page(uintptr_t virtual_addr, uintptr_t physical_addr, uint32_t flags);
vmm_unmap_page(uintptr_t virtual_addr);
vmm_get_physical(uintptr_t virtual_addr);
```

Once we have that, the heap can grow into a proper virtual heap region instead of depending on identity-mapped physical memory.

---

# Resources

- [Chapter 06 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_06)
- [GCC freestanding and hosted environments][1]
- [Intel 64 and IA-32 Software Developer Manuals][2]
- [OSDev Wiki: Memory Allocation](https://wiki.osdev.org/Memory_Allocation)
- [OSDev Wiki: Kernel Heap](https://wiki.osdev.org/Kernel_Heap)

That completes the sixth Toyix milestone: adding the first kernel heap and making dynamic kernel allocation possible.

Happy Coding!

[1]: https://gcc.gnu.org/onlinedocs/gcc/Standards.html "Standards (Using the GNU Compiler Collection (GCC))"
[2]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel® 64 and IA-32 Architectures"
