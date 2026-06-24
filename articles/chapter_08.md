# Chapter 8 - Moving the Heap onto Virtual Memory

In Chapter 6, the kernel heap still depended on low identity-mapped physical pages. That worked as a bootstrap step, but it tied the heap to the first 16 MiB of RAM.

In Chapter 7, we added a real VMM and proved that the kernel can map and unmap pages anywhere the page tables allow. That gives us the missing piece: the heap can now live in a high virtual address range and pull in physical pages only when it needs them.

This chapter finishes that transition.

The heap no longer treats physical addresses as direct pointers. Instead, it asks a small kernel-level virtual-memory wrapper to map physical pages into a heap-owned virtual range:

```c
vmem_map_page(vaddr, paddr, VMEM_FLAG_WRITABLE);
vmem_unmap_page(vaddr);
vmem_get_physical(vaddr);
```

That wrapper keeps the heap free of x86-specific details while still using the Chapter 7 VMM underneath.

---

## 1. What Changed

We add:

```text
include/kernel/vmem.h
kernel/vmem.c
```

We update:

```text
include/kernel/heap.h
kernel/heap.c
kernel/kmain.c
Makefile
tests/smoke.sh
README.md
CHANGELOG.md
index.md
```

The important architectural shift is simple:

```text
PMM allocates physical pages
VMM maps them into virtual pages
heap manages blocks in a high virtual region
```

---

## 2. Why Add `kernel/vmem`

The architecture-specific VMM still lives in:

```text
arch/x86/vmm.c
arch/x86/vmm.h
```

That is the right place for page-table operations, CR3 reloads, and `invlpg`.

But the heap should not care about x86 page flags. So the kernel gets a thin wrapper:

```text
include/kernel/vmem.h
kernel/vmem.c
```

The wrapper translates generic kernel flags into architecture flags and forwards the request to the x86 VMM.

### `include/kernel/vmem.h`

```c
// include/kernel/vmem.h
#ifndef TOYIX_KERNEL_VMEM_H
#define TOYIX_KERNEL_VMEM_H

#include <stdint.h>

#define VMEM_OK                 0
#define VMEM_ERR_INVALID       -1
#define VMEM_ERR_NO_MEMORY     -2
#define VMEM_ERR_ALREADY_MAPPED -3
#define VMEM_ERR_NOT_MAPPED    -4

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
void vmem_test_once(void);

#endif
```

### `kernel/vmem.c`

```c
// kernel/vmem.c
#include <stdint.h>
#include "arch/x86/paging.h"
#include "arch/x86/vmm.h"
#include "kernel/vmem.h"

static uint32_t vmem_to_arch_flags(uint32_t flags) {
	uint32_t arch_flags = 0;

	if ((flags & VMEM_FLAG_WRITABLE) != 0) {
		arch_flags |= X86_PAGE_WRITABLE;
	}

	if ((flags & VMEM_FLAG_USER) != 0) {
		arch_flags |= X86_PAGE_USER;
	}

	return arch_flags;
}

static int vmem_from_arch_result(int result) {
	switch (result) {
	case VMM_OK:
		return VMEM_OK;
	case VMM_ERR_INVALID:
		return VMEM_ERR_INVALID;
	case VMM_ERR_NO_MEMORY:
		return VMEM_ERR_NO_MEMORY;
	case VMM_ERR_ALREADY_MAPPED:
		return VMEM_ERR_ALREADY_MAPPED;
	case VMM_ERR_NOT_MAPPED:
		return VMEM_ERR_NOT_MAPPED;
	default:
		return VMEM_ERR_INVALID;
	}
}

void vmem_init(void) {
	vmm_init();
}

int vmem_map_page(
	uintptr_t virtual_addr,
	uintptr_t physical_addr,
	uint32_t flags
) {
	return vmem_from_arch_result(
		vmm_map_page(virtual_addr, physical_addr, vmem_to_arch_flags(flags))
	);
}

int vmem_unmap_page(uintptr_t virtual_addr) {
	return vmem_from_arch_result(vmm_unmap_page(virtual_addr));
}

uintptr_t vmem_get_physical(uintptr_t virtual_addr) {
	return vmm_get_physical(virtual_addr);
}

void vmem_test_once(void) {
	vmm_test_once();
}
```

That wrapper is intentionally small. It keeps the kernel layer portable without hiding the fact that the current implementation is still x86 underneath.

---

## 3. The New Heap Shape

The heap now lives in a dedicated high virtual range:

```text
HEAP_VIRTUAL_BASE  = 0xD1000000
HEAP_VIRTUAL_LIMIT = 0xD2000000
```

The heap code keeps track of:

```c
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
```

That is reflected in `include/kernel/heap.h`.

### `include/kernel/heap.h`

```c
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
```

The important part is that the heap now reasons in virtual addresses, not low physical frames.

---

## 4. How the Heap Grows

The heap maps physical pages on demand.

The growth path in `kernel/heap.c` is:

```text
heap_add_region()
  ↓
heap_map_virtual_region()
  ↓
pmm_alloc_page()
  ↓
vmem_map_page()
  ↓
zero the new page through the virtual address
```

That is the relevant part of the implementation:

```c
static int heap_map_virtual_region(uintptr_t start, uint32_t page_count) {
	for (uint32_t i = 0; i < page_count; ++i) {
		uintptr_t vaddr = start + ((uintptr_t)i * PMM_PAGE_SIZE);
		uintptr_t phys = pmm_alloc_page();

		if (phys == PMM_INVALID_PAGE) {
			heap_unmap_virtual_region(start, i);
			return -1;
		}

		int result = vmem_map_page(vaddr, phys, VMEM_FLAG_WRITABLE);

		if (result != VMEM_OK) {
			pmm_free_page(phys);
			heap_unmap_virtual_region(start, i);
			return -1;
		}

		memset((void *)vaddr, 0, PMM_PAGE_SIZE);
	}

	return 0;
}
```

The allocator can now grow by asking for any available physical page, then mapping it into the next heap virtual page.

That is the practical difference between a bootstrap heap and a real virtual-memory-backed heap.

---

## 5. Alignment and Block Layout

The heap block header includes a reserved field so the payload stays properly aligned:

```c
typedef struct heap_block {
	uint32_t magic;
	uint32_t size;
	uint32_t is_free;
	uint32_t reserved;
	struct heap_block *next;
	struct heap_block *prev;
} heap_block_t;
```

The returned pointer is always:

```c
(void *)((uintptr_t)block + heap_header_size())
```

and `kfree()` reverses that using the same header size.

That keeps payload pointers aligned to `HEAP_ALIGNMENT`, even though the block header itself is larger than a page-aligned boundary would otherwise guarantee.

---

## 6. Boot Order

`kernel/kmain.c` now initializes the layers in this order:

```c
pmm_init(mbi);
pmm_test_once();

paging_init();
paging_test_identity_mapping();

vmem_init();
vmem_test_once();

heap_init(4);
heap_test_once();
```

The current file also ends in the normal idle loop:

```c
kernel_idle();
```

So the boot path is still:

```text
PMM
paging
VMEM wrapper
heap
interrupts
timer
keyboard
idle
```

### `kernel/kmain.c`

The relevant boot sequence is:

```c
pmm_init(mbi);
pmm_test_once();

paging_init();
paging_test_identity_mapping();

vmem_init();
vmem_test_once();

heap_init(4);
heap_test_once();

kernel_idle();
```

That is the order the kernel uses today.

---

## 7. Makefile and Smoke Test

The build now includes the wrapper object:

```text
build/kernel/vmem.o
```

The object list also still includes the x86 VMM object:

```text
build/arch/x86/vmm.o
```

The smoke test checks the actual messages emitted by the current code:

```make
grep -q "Toyix kernel alive" build/test.log
grep -q "Boot protocol: Multiboot OK" build/test.log
grep -q "PMM: parsing Multiboot memory map" build/test.log
grep -q "PMM test: allocation/free sanity check passed" build/test.log
grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
grep -q "Paging test: identity-mapped kernel data is readable/writable" build/test.log
grep -q "Heap: initialized virtual heap with 4 page(s)" build/test.log
grep -q "Heap test: VMM-backed allocation/free sanity check passed" build/test.log
grep -q "Interrupts: enabled" build/test.log
grep -q "Timer: observed 3 ticks" build/test.log
grep -q "VMM: initialized kernel address-space mapper" build/test.log
grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
```

That is the right level of coverage for this chapter.

---

## 8. What Success Looks Like

A successful boot now produces output in this shape:

```text
Paging: enabled with identity map of first 16 MiB
Paging test: identity-mapped kernel data is readable/writable
VMM: initialized kernel address-space mapper
VMM test: map/translate/write/unmap sanity check passed
Heap: initialized virtual heap with 4 page(s)
Heap test: starting virtual heap test
Heap test: VMM-backed allocation/free sanity check passed
Interrupts: enabled
Timer: observed 3 ticks
```

The key proof is that the heap can now allocate a large block that forces growth, map more physical pages into the heap virtual range, and still return aligned pointers.

---

## 9. Common Failures

### Failure: `heap_init could not map initial heap region`

Likely causes:

```text
PMM is out of pages
VMM page-table creation failed
heap virtual base overlaps another mapping
```

### Failure: `kfree pointer outside heap virtual region`

That means the heap got a pointer that was never returned by `kmalloc()` or `kcalloc()`.

### Failure: `heap test returned unaligned allocation`

The heap header and payload offset are no longer paired correctly. Check that `heap_header_size()` is used everywhere, not raw `sizeof(heap_block_t)`.

### Failure: `VMM_ERR_ALREADY_MAPPED`

The heap tried to map a virtual page that is already in use. That usually means the heap virtual cursor did not advance correctly.

---

## 10. What This Chapter Gives Us

Before this chapter:

```text
heap pointer == low physical address
```

After this chapter:

```text
heap pointer == high virtual address
physical backing can come from anywhere PMM allows
```

The memory stack is now:

```text
physical RAM
  ↓
PMM tracks free frames
  ↓
VMM maps frames into virtual memory
  ↓
vmem provides a generic kernel interface
  ↓
heap manages blocks in a virtual heap range
```

That is the correct layer boundary for the rest of the system.

---

## 11. Next Step

With a VMM-backed heap in place, the next chapter can start using the heap as a normal kernel service instead of a bootstrap compromise. That opens the door to more allocator refinement, better diagnostics, and eventually higher-level kernel data structures.

---

## Resources

- [Chapter 08 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_08)
- [Intel 64 and IA-32 Software Developer Manuals][1]
- [OSDev Wiki: Paging](https://wiki.osdev.org/Paging)
- [OSDev Wiki: Page Tables](https://wiki.osdev.org/Page_Tables)
- [OSDev Wiki: Heap Allocation](https://wiki.osdev.org/Heap)
- [OSDev Wiki: Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation)

That completes Chapter 8: the heap now runs on top of virtual memory instead of leaning directly on low identity-mapped physical pages.

Happy Coding!

[1]: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html "Manuals for Intel(R) 64 and IA-32 Architectures"
