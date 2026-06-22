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
