# Chapter 9 — Cooperative Multitasking and Kernel Threads

We now have enough foundation to start multitasking:

``` text
console
GDT / IDT
exceptions
PIC / PIT / keyboard IRQs
PMM
paging
VMM
VMM-backed heap
```

This chapter adds the first scheduler.

Not a preemptive scheduler yet.

A **cooperative** scheduler.

That means each kernel thread keeps running until it voluntarily calls:

``` c 
thread_yield();
```

For cooperative multitasking, a task voluntarily gives up the CPU instead of being forcibly interrupted by a timer; OSDev describes cooperative multitasking exactly this way, and contrasts it with preemptive multitasking. ([OSDev Wiki][1])

We will build:

``` text
kernel thread object
per-thread kernel stack
ready queue
x86 context switch assembly
thread_create()
thread_yield()
thread_exit()
round-robin cooperative scheduling
two-thread test
```

The milestone for this chapter is:

``` text 
create two kernel threads
switch between them cooperatively
prove both threads run and return to the bootstrap thread
```

---

# 1. Why cooperative first?

Preemptive scheduling is where a timer interrupt forcibly interrupts the current thread and switches to another one.

That is where we eventually want to go.

But preemptive scheduling has more moving parts:

``` text 
timer IRQ frame
register save/restore
interrupt return path
scheduler locking
current thread kernel stack
nested interrupts
critical sections
```

Cooperative scheduling is simpler. The switch happens at a known point:

``` c 
thread_yield();
```

That lets us verify the core context-switch mechanism first.

OSDev’s context-switching page summarizes the essential idea: a context switch saves the old execution state and restores another execution state. Depending on the design, that may include instruction pointer, registers, segment state, CR3, FPU/SSE state, and more. ([OSDev Wiki][2])

Our first switch will save only what we need for cooperative **kernel threads in one address space**:

```text 
ESP
EBP
EBX
ESI
EDI
```

Why not everything?

Because a normal C function call already treats some registers as caller-saved. Our context switch is called like a normal function, so the compiler already assumes `EAX`, `ECX`, and `EDX` may be clobbered. The callee-saved registers are the important ones to preserve across the switch.

Later, preemptive scheduling from an interrupt will need a different path that accounts for the full interrupt frame.

---

# 2. What this chapter adds

Add:

```text 
arch/x86/
└── context_switch.asm

include/kernel/
└── thread.h

kernel/
└── thread.c
```

Modify:

```text 
kernel/kmain.c
Makefile
tests/smoke.sh
```

After this chapter, the boot flow becomes:

```text 
kernel_main()
  ↓
memory systems initialized
  ↓
threading_init()
  ↓
create worker A
create worker B
  ↓
bootstrap thread yields
  ↓
worker A runs
worker B runs
worker A runs
worker B runs
  ↓
workers exit
  ↓
bootstrap thread resumes
```

---

# 3. How a kernel thread is represented

A thread needs:

```text 
id
name
state
saved CPU context
kernel stack
entry function
argument pointer
ready-queue links
```

The most important part is the stack.

Each thread must have its own stack. Even in later interrupt-driven designs, interrupt handlers normally run on the stack of the thread they interrupted until privilege transitions or special interrupt stacks are introduced; OSDev forum discussions commonly emphasize that each thread needs its own kernel stack for sane switching. ([OSDev Forums][3])

For now, every kernel thread gets a stack from `kmalloc()`.

---

# 4. Add `include/kernel/thread.h`

```c 
// include/kernel/thread.h
#ifndef TOYIX_KERNEL_THREAD_H
#define TOYIX_KERNEL_THREAD_H

#include <stdint.h>

#define THREAD_STACK_SIZE 16384u

typedef void (*thread_entry_t)(void *arg);

typedef enum thread_state {
    THREAD_NEW = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_FINISHED
} thread_state_t;

typedef struct thread_context {
    uint32_t esp;
} thread_context_t;

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

    struct thread *next;
    struct thread *prev;
} thread_t;

void threading_init(void);

thread_t *thread_create(
    const char *name,
    thread_entry_t entry,
    void *arg
);

thread_t *thread_current(void);

void thread_yield(void);
void thread_exit(void) __attribute__((noreturn));

uint32_t thread_ready_count(void);
void thread_dump_state(void);
void thread_test_once(void);

#endif
```

## Why `thread_context_t` only stores `esp`

This looks suspiciously small:

```c 
typedef struct thread_context {
    uint32_t esp;
} thread_context_t;
```

But the trick is that the rest of the saved state lives **on the thread’s stack**.

The assembly switch routine pushes callee-saved registers onto the old thread’s stack, saves the old `ESP`, loads the new thread’s saved `ESP`, pops that thread’s saved registers, and returns.

So `thread_context_t.esp` points to the top of a saved stack frame.

This is a common software context-switch strategy.

---

# 5. Add `arch/x86/context_switch.asm`

```asm 
; arch/x86/context_switch.asm
;
; Cooperative kernel-thread context switch.
;
; C prototype:
;
;   void x86_context_switch(
;       thread_context_t *old_context,
;       thread_context_t *new_context
;   );
;
; thread_context_t currently contains only:
;
;   uint32_t esp;
;
; The rest of the saved state lives on the stack.
;
; Stack layout expected for a newly created thread:
;
;   [esp +  0] saved EDI
;   [esp +  4] saved ESI
;   [esp +  8] saved EBX
;   [esp + 12] saved EBP
;   [esp + 16] return address
;
; After loading ESP, this routine pops EDI/ESI/EBX/EBP and RETs into the
; thread bootstrap function.

BITS 32

global x86_context_switch

x86_context_switch:
    ; Arguments before we push anything:
    ;   [esp + 4] = old_context
    ;   [esp + 8] = new_context
    mov eax, [esp + 4]
    mov edx, [esp + 8]

    ; Save callee-saved registers on the old thread's stack.
    push ebp
    push ebx
    push esi
    push edi

    ; Save old ESP into old_context->esp.
    mov [eax], esp

    ; Load new ESP from new_context->esp.
    mov esp, [edx]

    ; Restore callee-saved registers from the new thread's stack.
    pop edi
    pop esi
    pop ebx
    pop ebp

    ; Continue in the new thread.
    ret
```

## What this assembly actually does

Suppose thread A calls:

```c 
thread_yield();
```

Eventually, C calls:

```c 
x86_context_switch(&thread_a->context, &thread_b->context);
```

The assembly does this:

```text 
push A's EBP, EBX, ESI, EDI
save A's ESP
load B's ESP
pop B's EDI, ESI, EBX, EBP
ret into B
```

When thread B later yields back to A, the reverse happens.

The strange part is that `x86_context_switch()` does not return to the same thread that called it. It returns in whichever thread is selected next.

That is the heart of software context switching.

---

# 6. Add `kernel/thread.c`

```c 
// kernel/thread.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/thread.h"

#define THREAD_MAGIC 0x54485244u

extern void x86_context_switch(
    thread_context_t *old_context,
    thread_context_t *new_context
);

static thread_t bootstrap_thread;

static thread_t *current_thread;
static thread_t *ready_head;
static thread_t *ready_tail;

static uint32_t next_thread_id;
static uint32_t finished_thread_count;

static void thread_bootstrap(void);

static uintptr_t align_down(uintptr_t value, uintptr_t alignment) {
    return value & ~(alignment - 1u);
}

static void validate_thread(const thread_t *thread) {
    if (thread == 0) {
        kernel_panic("thread: null thread pointer");
    }

    if (thread->magic != THREAD_MAGIC) {
        kernel_panic("thread: magic mismatch");
    }
}

static const char *thread_state_name(thread_state_t state) {
    switch (state) {
        case THREAD_NEW:
            return "NEW";

        case THREAD_READY:
            return "READY";

        case THREAD_RUNNING:
            return "RUNNING";

        case THREAD_FINISHED:
            return "FINISHED";

        default:
            return "UNKNOWN";
    }
}

static void ready_push(thread_t *thread) {
    validate_thread(thread);

    thread->next = 0;
    thread->prev = ready_tail;

    if (ready_tail != 0) {
        ready_tail->next = thread;
    } else {
        ready_head = thread;
    }

    ready_tail = thread;
}

static thread_t *ready_pop(void) {
    thread_t *thread = ready_head;

    if (thread == 0) {
        return 0;
    }

    ready_head = thread->next;

    if (ready_head != 0) {
        ready_head->prev = 0;
    } else {
        ready_tail = 0;
    }

    thread->next = 0;
    thread->prev = 0;

    return thread;
}

uint32_t thread_ready_count(void) {
    uint32_t count = 0;

    for (thread_t *t = ready_head; t != 0; t = t->next) {
        validate_thread(t);
        count++;
    }

    return count;
}

static void thread_make_initial_stack(thread_t *thread) {
    validate_thread(thread);

    uintptr_t stack_top =
        (uintptr_t)thread->stack_base + thread->stack_size;

    /*
     * Keep the stack reasonably aligned.
     *
     * We are not using SSE or ABI-heavy code here, but 16-byte alignment is a
     * good habit and will matter more later.
     */
    stack_top = align_down(stack_top, 16u);

    uint32_t *sp = (uint32_t *)stack_top;

    /*
     * This is the artificial stack frame expected by context_switch.asm:
     *
     *   pop edi
     *   pop esi
     *   pop ebx
     *   pop ebp
     *   ret
     */
    *(--sp) = (uint32_t)thread_bootstrap;  // return address
    *(--sp) = 0;                           // saved EBP
    *(--sp) = 0;                           // saved EBX
    *(--sp) = 0;                           // saved ESI
    *(--sp) = 0;                           // saved EDI

    thread->context.esp = (uint32_t)sp;
}

void threading_init(void) {
    ready_head = 0;
    ready_tail = 0;
    next_thread_id = 1;
    finished_thread_count = 0;

    bootstrap_thread.magic = THREAD_MAGIC;
    bootstrap_thread.id = 0;
    bootstrap_thread.name = "bootstrap";
    bootstrap_thread.state = THREAD_RUNNING;
    bootstrap_thread.context.esp = 0;
    bootstrap_thread.stack_base = 0;
    bootstrap_thread.stack_size = 0;
    bootstrap_thread.entry = 0;
    bootstrap_thread.arg = 0;
    bootstrap_thread.next = 0;
    bootstrap_thread.prev = 0;

    current_thread = &bootstrap_thread;

    console_writeln("Threads: cooperative scheduler initialized");
}

thread_t *thread_create(
    const char *name,
    thread_entry_t entry,
    void *arg
) {
    if (entry == 0) {
        kernel_panic("thread_create received null entry");
    }

    thread_t *thread = (thread_t *)kcalloc(1, sizeof(thread_t));

    if (thread == 0) {
        kernel_panic("thread_create could not allocate thread object");
    }

    void *stack = kmalloc(THREAD_STACK_SIZE);

    if (stack == 0) {
        kernel_panic("thread_create could not allocate kernel stack");
    }

    thread->magic = THREAD_MAGIC;
    thread->id = next_thread_id++;
    thread->name = name != 0 ? name : "unnamed";
    thread->state = THREAD_NEW;

    thread->stack_base = stack;
    thread->stack_size = THREAD_STACK_SIZE;

    thread->entry = entry;
    thread->arg = arg;

    thread->next = 0;
    thread->prev = 0;

    thread_make_initial_stack(thread);

    thread->state = THREAD_READY;
    ready_push(thread);

    console_write("Thread: created ");
    console_write(thread->name);
    console_write(" id=");
    console_write_u32_dec(thread->id);
    console_write(" stack=");
    console_write_hex32((uint32_t)(uintptr_t)thread->stack_base);
    console_putc('\n');

    return thread;
}

thread_t *thread_current(void) {
    return current_thread;
}

void thread_yield(void) {
    thread_t *old_thread = current_thread;
    validate_thread(old_thread);

    thread_t *next_thread = ready_pop();

    if (next_thread == 0) {
        return;
    }

    validate_thread(next_thread);

    if (old_thread->state == THREAD_RUNNING) {
        old_thread->state = THREAD_READY;
        ready_push(old_thread);
    }

    next_thread->state = THREAD_RUNNING;
    current_thread = next_thread;

    x86_context_switch(&old_thread->context, &next_thread->context);
}

void thread_exit(void) {
    thread_t *old_thread = current_thread;
    validate_thread(old_thread);

    console_write("Thread: exiting ");
    console_write(old_thread->name);
    console_write(" id=");
    console_write_u32_dec(old_thread->id);
    console_putc('\n');

    old_thread->state = THREAD_FINISHED;
    finished_thread_count++;

    thread_t *next_thread = ready_pop();

    if (next_thread == 0) {
        kernel_panic("thread_exit: no thread to switch to");
    }

    validate_thread(next_thread);

    next_thread->state = THREAD_RUNNING;
    current_thread = next_thread;

    x86_context_switch(&old_thread->context, &next_thread->context);

    kernel_panic("thread_exit returned unexpectedly");
}

static void thread_bootstrap(void) {
    thread_t *self = current_thread;
    validate_thread(self);

    if (self->entry == 0) {
        kernel_panic("thread_bootstrap found null entry");
    }

    self->entry(self->arg);

    thread_exit();
}

void thread_dump_state(void) {
    console_write("Threads: current=");
    console_write(current_thread != 0 ? current_thread->name : "(null)");
    console_write(" ready=");
    console_write_u32_dec(thread_ready_count());
    console_write(" finished=");
    console_write_u32_dec(finished_thread_count);
    console_putc('\n');

    for (thread_t *t = ready_head; t != 0; t = t->next) {
        validate_thread(t);

        console_write("  ready thread id=");
        console_write_u32_dec(t->id);
        console_write(" name=");
        console_write(t->name);
        console_write(" state=");
        console_write(thread_state_name(t->state));
        console_putc('\n');
    }
}

typedef struct thread_test_arg {
    const char *label;
    uint32_t iterations;
} thread_test_arg_t;

static void worker_thread(void *arg) {
    thread_test_arg_t *test = (thread_test_arg_t *)arg;

    for (uint32_t i = 0; i < test->iterations; ++i) {
        console_write("Thread test: worker ");
        console_write(test->label);
        console_write(" step ");
        console_write_u32_dec(i);
        console_putc('\n');

        thread_yield();
    }

    console_write("Thread test: worker ");
    console_write(test->label);
    console_writeln(" done");
}

void thread_test_once(void) {
    console_writeln("Thread test: starting cooperative multitasking test");

    static thread_test_arg_t arg_a = {
        .label = "A",
        .iterations = 3
    };

    static thread_test_arg_t arg_b = {
        .label = "B",
        .iterations = 3
    };

    thread_create("worker-a", worker_thread, &arg_a);
    thread_create("worker-b", worker_thread, &arg_b);

    while (thread_ready_count() > 0) {
        thread_yield();
    }

    console_writeln("Thread test: completed cooperative multitasking test");
    thread_dump_state();
}
```

---

# 7. How the first thread starts

A newly created thread has never called `thread_yield()`.

So it has no naturally saved stack frame.

We fake one.

This code builds the stack:

```c 
*(--sp) = (uint32_t)thread_bootstrap;  // return address
*(--sp) = 0;                           // saved EBP
*(--sp) = 0;                           // saved EBX
*(--sp) = 0;                           // saved ESI
*(--sp) = 0;                           // saved EDI
```

The context switch routine expects:

```asm 
pop edi
pop esi
pop ebx
pop ebp
ret
```

So the first time the scheduler switches to a new thread, the `ret` does not return to a previous call site. It jumps into:

```c 
thread_bootstrap()
```

Then `thread_bootstrap()` calls the thread’s real entry function:

```c 
self->entry(self->arg);
```

When the entry function returns, `thread_bootstrap()` calls:

```c 
thread_exit();
```

That prevents a kernel thread from falling off the end into random memory.

---

# 8. Why the bootstrap thread exists

Before we create any kernel threads, the CPU is already executing `kernel_main()` on the original boot stack from `boot.asm`.

That execution context is also a thread, even though we did not create it through `thread_create()`.

We call it:

```text 
bootstrap
```

The bootstrap thread has:

```text 
id = 0
name = "bootstrap"
state = RUNNING
no allocated stack object
```

When it first calls `thread_yield()`, the context switch assembly saves its current `ESP` into:

```c 
bootstrap_thread.context.esp
```

After worker threads yield back, the bootstrap thread resumes exactly after the `thread_yield()` call.

That is why `thread_test_once()` can create workers, yield repeatedly, and then continue.

---

# 9. Update `kernel/kmain.c`

Add:

```c 
#include "kernel/thread.h"
```

Then initialize and test threading after the heap is working.

Here is the full updated file.

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
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/thread.h"
#include "kernel/vmem.h"

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

    vmem_init();
    vmem_test_once();

    heap_init(4);
    heap_test_once();

    threading_init();
    thread_test_once();

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
    console_writeln("Next stop: timer-driven preemptive scheduling.");

    kernel_idle_forever();
}
```

## Why threading is tested before enabling timer interrupts

This chapter is about cooperative switching.

So we test it before:

```c 
interrupts_enable();
```

That removes timer IRQ scheduling from the equation.

Later, when we add preemption, IRQ0 will call into the scheduler. That is a different and more delicate path.

---

# 10. Update `Makefile`

Add:

```text id="j6yxw9"
build/arch/x86/context_switch.o
build/kernel/thread.o
```

to the object list.

The relevant `OBJS` section becomes:

```make 
OBJS := \
    build/arch/x86/boot.o \
    build/arch/x86/context_switch.o \
    build/arch/x86/gdt.o \
    build/arch/x86/gdt_flush.o \
    build/arch/x86/idt.o \
    build/arch/x86/interrupts.o \
    build/arch/x86/isr.o \
    build/arch/x86/irq.o \
    build/arch/x86/paging.o \
    build/arch/x86/pic.o \
    build/arch/x86/pit.o \
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/thread.o \
    build/kernel/vmem.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the `test` target with threading checks:

```make 
	grep -q "Threads: cooperative scheduler initialized" build/test.log
	grep -q "Thread test: worker A step 0" build/test.log
	grep -q "Thread test: worker B step 0" build/test.log
	grep -q "Thread test: completed cooperative multitasking test" build/test.log
```

The important part of the test target should now include:

```make 
test: iso
	$(GRUB_FILE) --is-x86-multiboot build/kernel.elf
	@mkdir -p build
	@timeout 5s $(QEMU) \
		-cdrom build/toyix.iso \
		-serial stdio \
		-display none \
		-monitor none \
		-no-reboot \
		> build/test.log || true
	grep -q "Toyix kernel alive" build/test.log
	grep -q "Boot protocol: Multiboot OK" build/test.log
	grep -q "PMM test: allocation/free sanity check passed" build/test.log
	grep -q "Paging: enabled with identity map of first 16 MiB" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	grep -q "Heap test: VMM-backed allocation/free sanity check passed" build/test.log
	grep -q "Threads: cooperative scheduler initialized" build/test.log
	grep -q "Thread test: worker A step 0" build/test.log
	grep -q "Thread test: worker B step 0" build/test.log
	grep -q "Thread test: completed cooperative multitasking test" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, memory, heap, and cooperative threading smoke test passed."
```

---

# 11. Update `tests/smoke.sh`

```bash 
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 9 checks passed."
```

Run:

```bash id="ymwa3f"
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 12. Expected output

A successful boot should now include something like:

```text 
Threads: cooperative scheduler initialized
Thread test: starting cooperative multitasking test
Thread: created worker-a id=1 stack=0xD100XXXX
Thread: created worker-b id=2 stack=0xD100XXXX
Thread test: worker A step 0
Thread test: worker B step 0
Thread test: worker A step 1
Thread test: worker B step 1
Thread test: worker A step 2
Thread test: worker B step 2
Thread test: worker A done
Thread: exiting worker-a id=1
Thread test: worker B done
Thread: exiting worker-b id=2
Thread test: completed cooperative multitasking test
Threads: current=bootstrap ready=0 finished=2
```

The exact stack addresses will vary.

The important fact is that worker A and worker B alternate. That proves the scheduler is switching stacks.

---

# 13. What just happened mechanically?

Let’s trace the first few switches.

Initial state:

```text 
current = bootstrap
ready queue = worker-a, worker-b
```

Bootstrap calls:

```c 
thread_yield();
```

Scheduler chooses worker A:

```text 
current = worker-a
ready queue = worker-b, bootstrap
```

The context switch saves bootstrap’s `ESP` and loads worker A’s `ESP`.

Worker A has never run before, so its fake stack frame causes `ret` to enter:

```c 
thread_bootstrap()
```

Worker A prints:

```text 
Thread test: worker A step 0
```

Then worker A yields.

Scheduler chooses worker B:

```text 
current = worker-b
ready queue = bootstrap, worker-a
```

Worker B also starts through `thread_bootstrap()`.

Eventually the bootstrap thread is scheduled again. Its original `thread_yield()` returns, and the loop in `thread_test_once()` continues.

This proves that `thread_yield()` is not a normal function call in the ordinary sense. It may return much later, after other threads have run.

---

# 14. Why we do not free finished thread stacks yet

When a thread calls `thread_exit()`, it is still running on its own stack.

If it tried to `kfree()` that stack immediately, it would be freeing the memory currently being used to execute `thread_exit()`.

That is dangerous.

So for now, finished threads are marked:

```c 
THREAD_FINISHED
```

and their stacks are intentionally not reclaimed.

Later, we need a reaper:

```text 
thread exits
  ↓
thread marked ZOMBIE / FINISHED
  ↓
scheduler switches away
  ↓
separate reaper frees stack and thread object
```

That is the safe pattern.

---

# 15. Why this is not preemptive yet

The timer interrupt is still only incrementing ticks.

It does not call the scheduler.

That means a badly behaved thread can still do this:

```c id="czhjme"
for (;;) {
    // never yields
}
```

and no other thread will run.

That is the defining limitation of cooperative scheduling.

Preemptive scheduling fixes that by switching threads from the timer interrupt path. But doing that correctly requires care because the saved state lives in an interrupt frame, not just in the normal function-call stack.

OSDev’s context-switching discussion notes that a full context switch may involve more than general registers, including CR3, FPU/MMX/SSE state, and other CPU state depending on the OS design. ([OSDev Wiki][2])

For this tutorial kernel, we deliberately postpone those complications.

---

# 16. Common failures

## Failure: instant crash when switching to worker A

Likely cause:

```text 
initial stack layout does not match context_switch.asm
```

Check that the stack is built in this exact order:

```c 
*(--sp) = (uint32_t)thread_bootstrap;
*(--sp) = 0; // EBP
*(--sp) = 0; // EBX
*(--sp) = 0; // ESI
*(--sp) = 0; // EDI
```

and that assembly restores in this exact order:

```asm 
pop edi
pop esi
pop ebx
pop ebp
ret
```

Those two must match.

## Failure: worker A runs, but worker B never runs

Likely causes:

```text 
ready queue links are broken
old thread is not requeued during yield
ready_pop() loses tail pointer
```

Check this part of `thread_yield()`:

```c 
if (old_thread->state == THREAD_RUNNING) {
    old_thread->state = THREAD_READY;
    ready_push(old_thread);
}
```

If the old running thread is not pushed to the back, round-robin scheduling will not happen.

## Failure: `thread_exit: no thread to switch to`

That means a non-bootstrap thread exited when no other thread was ready.

In this chapter’s test, bootstrap should always be in the ready queue when a worker is running. If it is not, `thread_yield()` likely failed to requeue bootstrap before switching to the worker.

## Failure: stack addresses are low or strange

They should normally be in the heap’s high virtual region:

```text 
0xD1000000 - 0xD2000000
```

If stack addresses are low physical addresses, you may still be using the old Chapter 6 heap instead of the Chapter 8 VMM-backed heap.

---

# 17. What this chapter achieved

We now have:

```text 
kernel thread objects
per-thread kernel stacks
ready queue
cooperative scheduler
x86 software context switch
thread_create()
thread_yield()
thread_exit()
```

The kernel can now run multiple independent execution streams.

The stack now looks like this:

```text 
PMM
  ↓
VMM
  ↓
heap
  ↓
thread objects and stacks
  ↓
scheduler
  ↓
context switch assembly
```

That is the beginning of a real kernel execution model.

---

# 18. Commit this chapter

After tests pass:

```bash 
git status
git add .
git commit -m "Add cooperative kernel threads and context switching"
```

This is a critical checkpoint. Do not move into preemption until cooperative switching is stable.

---

# 19. Next chapter

The next step is **timer-driven preemption**.

We will refactor the scheduler so IRQ0 can request a reschedule.

The careful path is:

```text 
1. Keep cooperative yield working.
2. Add scheduler_tick() called by PIT IRQ.
3. Count time slices.
4. Add a need_reschedule flag.
5. First switch at safe kernel points.
6. Then move toward switching directly from interrupt frames.
```

The safest immediate next chapter is not “fully preempt from IRQ0 instantly.” It is:

```text 
scheduler clock ticks and time slices
```

Then the following chapter can perform real interrupt-frame preemption.

[1]: https://wiki.osdev.org/Multitasking_Systems?utm_source=chatgpt.com "Multitasking Systems"
[2]: https://wiki.osdev.org/Context_Switching?utm_source=chatgpt.com "Context Switching"
[3]: https://forum.osdev.org/viewtopic.php?t=56972&utm_source=chatgpt.com "Confused about context switch - OSDev.org"

---

# Resources

- [Chapter 9 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_09)
- [OSDev multitasking systems](https://wiki.osdev.org/Multitasking_Systems)
- [OSDev context switching](https://wiki.osdev.org/Context_Switching)
- [OSDev forum discussion on context switching](https://forum.osdev.org/viewtopic.php?t=56972)

# Closure

Chapter 9 adds the first cooperative kernel threads, a software context switch, and a round-robin scheduler path that proves the kernel can move execution between independent stacks. That gives us the foundation we need before we move on to timer-driven preemption and more advanced scheduling rules.

Happy Coding!
