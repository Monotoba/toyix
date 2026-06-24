// include/kernel/heap.h
#ifndef TOYIX_KERNEL_HEAP_H
#define TOYIX_KERNEL_HEAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct heap_stats {
	uint32_t region_bytes;
	uint32_t mapped_pages;
	
	uintptr_t virtual_base;
	uintptr_t virtual_next;
	uintptr_t virtual_limit;	
	
	uint32_t free_payload_bytes;
	uint32_t used_payload_bytes;
	uint32_t block_count;
	
	uint32_t active_allocations;
	uint32_t total_allocations;
} heap_stats_t;

void heap_init(uint32_t initial_pages);

void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void kfree(void *ptr);

heap_stats_t heap_get_stats(void);
void heap_dump_stats(void);
void heap_test_once(void);

#endif
