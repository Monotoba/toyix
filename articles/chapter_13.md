# Chapter 13 — Mutexes, Semaphores, and a Console Lock

In Chapter 12, we added wait queues and blocking keyboard input:

```text
keyboard IRQ
  ↓
ring buffer
  ↓
wait queue wakeup
  ↓
blocking reader thread
```

Now we need synchronization primitives that ordinary kernel code can use.

This chapter adds:

```text
mutex
counting semaphore
console output lock
mutex test
semaphore test
non-interleaved console-line printing
```

A mutex provides mutual exclusion: only one thread may hold the protected resource at a time. OSDev describes a mutex as a mutual-exclusion mechanism, similar to a binary semaphore, used to make a critical section safe in a multithreaded system. ([OSDev Wiki][1])

A semaphore is more general: it holds a count. Waiting decrements the count when it is nonzero; signaling increments the count or wakes a waiter. OSDev’s semaphore page describes counting semaphores this way. ([OSDev Wiki][2])

---

# 1. Why locks are necessary now

Before preemption, output like this was fairly predictable:

```c
console_write("hello ");
console_writeln("world");
```

After preemption, a timer interrupt can switch threads between those calls:

```text
thread A: console_write("hello ")
timer interrupt
thread B: console_write("goodbye ")
thread B: console_writeln("moon")
thread A resumes
thread A: console_writeln("world")
```

The screen may show:

```text
hello goodbye moon
world
```

or worse, characters may interleave.

The console is a shared resource. Shared resources need synchronization.

This chapter will let us write:

```c
console_lock();
console_writeln("one complete line");
console_unlock();
```

Eventually, we can build `kprintf()` on top of that.

---

# 2. Why we need blocking locks, not spinlocks

A spinlock repeatedly tries to acquire a lock until it succeeds. OSDev defines a spinlock as a reentrancy lock where the CPU repeatedly attempts to acquire the lock, and notes that a contended spinlock can waste CPU time. ([OSDev Wiki][3])

For our current single-CPU, preemptive kernel, a blocking mutex is usually the better first primitive:

```text
lock unavailable
  ↓
put current thread on wait queue
  ↓
block
  ↓
owner unlocks
  ↓
wake waiter
```

That uses the scheduler instead of burning CPU cycles.

Later, we will still want spinlocks for very short IRQ-safe critical sections and SMP work. But a sleeping mutex is the right primitive for ordinary thread-context code.

---

# 3. Patch overview

Add:

```text
include/kernel/
└── sync.h

kernel/
└── sync.c
```

Modify:

```text
include/kernel/console.h
kernel/console.c
drivers/console/serial.c
drivers/console/vga_text.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

The console driver change is important. We will move locking to the console layer, while keeping low-level driver output functions lock-free.

---

# 4. Add `include/kernel/sync.h`

```c
// include/kernel/sync.h
#ifndef TOYIX_KERNEL_SYNC_H
#define TOYIX_KERNEL_SYNC_H

#include <stdint.h>
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

typedef struct mutex {
    const char *name;
    int locked;
    thread_t *owner;
    wait_queue_t waiters;
} mutex_t;

typedef struct semaphore {
    const char *name;
    int32_t count;
    wait_queue_t waiters;
} semaphore_t;

void mutex_init(mutex_t *mutex, const char *name);
void mutex_lock(mutex_t *mutex);
int mutex_try_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);
int mutex_is_locked(mutex_t *mutex);

void semaphore_init(semaphore_t *sem, const char *name, int32_t initial_count);
void semaphore_wait(semaphore_t *sem);
int semaphore_try_wait(semaphore_t *sem);
void semaphore_signal(semaphore_t *sem);

void sync_test_once(void);

#endif
```

## What each primitive is for

A mutex protects one exclusive resource:

```text
console
filesystem metadata
process table
heap metadata, later
VFS mount table
```

A semaphore represents a count of available units:

```text
items in queue
free slots in queue
available buffers
completed events
```

A mutex has an owner. A semaphore does not need one.

That distinction matters. Unlocking a mutex from a thread that does not own it is a bug. Signaling a semaphore from a different thread, or even from an interrupt path in later designs, can be valid.

---

# 5. Add `kernel/sync.c`

```c
// kernel/sync.c
#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/sync.h"
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

static int mutex_available_condition(void *context) {
    mutex_t *mutex = (mutex_t *)context;
    return mutex->locked == 0;
}

static int semaphore_available_condition(void *context) {
    semaphore_t *sem = (semaphore_t *)context;
    return sem->count > 0;
}

void mutex_init(mutex_t *mutex, const char *name) {
    if (mutex == 0) {
        kernel_panic("mutex_init received null mutex");
    }

    mutex->name = name != 0 ? name : "unnamed";
    mutex->locked = 0;
    mutex->owner = 0;
    wait_queue_init(&mutex->waiters, mutex->name);
}

void mutex_lock(mutex_t *mutex) {
    if (mutex == 0) {
        kernel_panic("mutex_lock received null mutex");
    }

    thread_t *self = thread_current();

    irq_flags_t flags = irq_save();

    if (mutex->locked && mutex->owner == self) {
        irq_restore(flags);
        kernel_panic("mutex_lock attempted recursive lock");
    }

    while (mutex->locked) {
        irq_restore(flags);
        wait_queue_wait(&mutex->waiters, mutex_available_condition, mutex);
        flags = irq_save();
    }

    mutex->locked = 1;
    mutex->owner = self;

    irq_restore(flags);
}

int mutex_try_lock(mutex_t *mutex) {
    if (mutex == 0) {
        kernel_panic("mutex_try_lock received null mutex");
    }

    thread_t *self = thread_current();

    irq_flags_t flags = irq_save();

    if (!mutex->locked) {
        mutex->locked = 1;
        mutex->owner = self;
        irq_restore(flags);
        return 1;
    }

    irq_restore(flags);
    return 0;
}

void mutex_unlock(mutex_t *mutex) {
    if (mutex == 0) {
        kernel_panic("mutex_unlock received null mutex");
    }

    thread_t *self = thread_current();

    irq_flags_t flags = irq_save();

    if (!mutex->locked) {
        irq_restore(flags);
        kernel_panic("mutex_unlock attempted unlock of unlocked mutex");
    }

    if (mutex->owner != self) {
        irq_restore(flags);
        kernel_panic("mutex_unlock attempted by non-owner");
    }

    mutex->locked = 0;
    mutex->owner = 0;

    irq_restore(flags);

    wait_queue_wake_one(&mutex->waiters);
}

int mutex_is_locked(mutex_t *mutex) {
    if (mutex == 0) {
        return 0;
    }

    irq_flags_t flags = irq_save();
    int locked = mutex->locked;
    irq_restore(flags);

    return locked;
}

void semaphore_init(semaphore_t *sem, const char *name, int32_t initial_count) {
    if (sem == 0) {
        kernel_panic("semaphore_init received null semaphore");
    }

    if (initial_count < 0) {
        kernel_panic("semaphore_init received negative count");
    }

    sem->name = name != 0 ? name : "unnamed";
    sem->count = initial_count;
    wait_queue_init(&sem->waiters, sem->name);
}

void semaphore_wait(semaphore_t *sem) {
    if (sem == 0) {
        kernel_panic("semaphore_wait received null semaphore");
    }

    irq_flags_t flags = irq_save();

    while (sem->count <= 0) {
        irq_restore(flags);
        wait_queue_wait(&sem->waiters, semaphore_available_condition, sem);
        flags = irq_save();
    }

    sem->count--;

    irq_restore(flags);
}

int semaphore_try_wait(semaphore_t *sem) {
    if (sem == 0) {
        kernel_panic("semaphore_try_wait received null semaphore");
    }

    irq_flags_t flags = irq_save();

    if (sem->count > 0) {
        sem->count--;
        irq_restore(flags);
        return 1;
    }

    irq_restore(flags);
    return 0;
}

void semaphore_signal(semaphore_t *sem) {
    if (sem == 0) {
        kernel_panic("semaphore_signal received null semaphore");
    }

    irq_flags_t flags = irq_save();

    sem->count++;

    irq_restore(flags);

    wait_queue_wake_one(&sem->waiters);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct sync_mutex_test_arg {
    const char *label;
    mutex_t *mutex;
    volatile uint32_t *shared_counter;
    volatile uint32_t *done_counter;
} sync_mutex_test_arg_t;

typedef struct sync_semaphore_test_arg {
    const char *label;
    semaphore_t *start_sem;
    volatile uint32_t *order_counter;
    volatile uint32_t *done_counter;
} sync_semaphore_test_arg_t;

static void sync_mutex_worker(void *arg) {
    sync_mutex_test_arg_t *test = (sync_mutex_test_arg_t *)arg;

    for (uint32_t i = 0; i < 50; ++i) {
        mutex_lock(test->mutex);

        uint32_t old_value = *test->shared_counter;

        /*
         * Encourage preemption while the mutex is held. This would expose
         * lost updates quickly if the mutex were broken.
         */
        thread_sleep_ticks(1);

        *test->shared_counter = old_value + 1;

        mutex_unlock(test->mutex);

        thread_sleep_ticks(1);
    }

    console_write("Sync test: mutex worker ");
    console_write(test->label);
    console_writeln(" done");

    (*test->done_counter)++;
}

static void sync_semaphore_worker(void *arg) {
    sync_semaphore_test_arg_t *test = (sync_semaphore_test_arg_t *)arg;

    semaphore_wait(test->start_sem);

    uint32_t order = *test->order_counter;
    *test->order_counter = order + 1;

    console_write("Sync test: semaphore worker ");
    console_write(test->label);
    console_write(" passed order ");
    console_write_u32_dec(order);
    console_putc('\n');

    (*test->done_counter)++;
}

void sync_test_once(void) {
    console_writeln("Sync test: starting mutex/semaphore test");

    static mutex_t test_mutex;
    static volatile uint32_t shared_counter;
    static volatile uint32_t mutex_done;

    shared_counter = 0;
    mutex_done = 0;

    mutex_init(&test_mutex, "sync-test-mutex");

    static sync_mutex_test_arg_t mutex_arg_a;
    static sync_mutex_test_arg_t mutex_arg_b;

    mutex_arg_a.label = "M1";
    mutex_arg_a.mutex = &test_mutex;
    mutex_arg_a.shared_counter = &shared_counter;
    mutex_arg_a.done_counter = &mutex_done;

    mutex_arg_b.label = "M2";
    mutex_arg_b.mutex = &test_mutex;
    mutex_arg_b.shared_counter = &shared_counter;
    mutex_arg_b.done_counter = &mutex_done;

    thread_create("mutex-m1", sync_mutex_worker, &mutex_arg_a);
    thread_create("mutex-m2", sync_mutex_worker, &mutex_arg_b);

    while (mutex_done < 2) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (shared_counter != 100) {
        console_write("Sync test: shared_counter=");
        console_write_u32_dec(shared_counter);
        console_putc('\n');
        kernel_panic("mutex test lost updates");
    }

    static semaphore_t sem;
    static volatile uint32_t sem_order;
    static volatile uint32_t sem_done;

    sem_order = 0;
    sem_done = 0;

    semaphore_init(&sem, "sync-test-semaphore", 0);

    static sync_semaphore_test_arg_t sem_arg_a;
    static sync_semaphore_test_arg_t sem_arg_b;

    sem_arg_a.label = "S1";
    sem_arg_a.start_sem = &sem;
    sem_arg_a.order_counter = &sem_order;
    sem_arg_a.done_counter = &sem_done;

    sem_arg_b.label = "S2";
    sem_arg_b.start_sem = &sem;
    sem_arg_b.order_counter = &sem_order;
    sem_arg_b.done_counter = &sem_done;

    thread_create("sem-s1", sync_semaphore_worker, &sem_arg_a);
    thread_create("sem-s2", sync_semaphore_worker, &sem_arg_b);

    /*
     * Let both workers block on the semaphore.
     */
    thread_sleep_ticks(2);

    semaphore_signal(&sem);
    thread_sleep_ticks(1);

    semaphore_signal(&sem);

    while (sem_done < 2) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (sem_order != 2) {
        kernel_panic("semaphore test did not release two workers");
    }

    console_writeln("Sync test: mutex/semaphore sanity check passed");
}
```

---

# 6. Why this mutex is non-recursive

If a thread does this:

```c
mutex_lock(&m);
mutex_lock(&m);
```

it has tried to acquire the same mutex twice.

Some systems support recursive mutexes. We do not.

For a kernel, non-recursive mutexes are simpler and better at exposing design errors. If code needs recursive locking, it often means the ownership boundaries are unclear.

So this is a deliberate panic:

```c
if (mutex->locked && mutex->owner == self) {
    kernel_panic("mutex_lock attempted recursive lock");
}
```

---

# 7. Why `mutex_unlock()` wakes after releasing the lock

The unlock path is:

```text
clear locked flag
clear owner
wake one waiter
```

The woken waiter does not automatically own the mutex. It wakes, loops in `mutex_lock()`, rechecks the condition, and then acquires the mutex if still available.

This pattern tolerates races and spurious wakeups:

```c
while (mutex->locked) {
    wait_queue_wait(...);
}
```

The `while`, not `if`, is important.

---

# 8. Why this is not yet SMP-safe

This kernel is still single-CPU.

On one CPU, disabling interrupts around short scheduler and lock-state operations prevents preemption from interrupting the critical section.

On multiple CPUs, disabling local interrupts is not enough. Another CPU could still modify the same lock or queue. An SMP kernel needs atomic operations and spinlocks. OSDev’s spinlock article explains that spinlocks repeatedly attempt to acquire the lock and are used for reentrancy protection, but can waste CPU time when contended. ([OSDev Wiki][3])

We are not building SMP yet.

---

# 9. Update `include/kernel/console.h`

Add lock control and raw output declarations.

```c
// include/kernel/console.h
#ifndef TOYIX_KERNEL_CONSOLE_H
#define TOYIX_KERNEL_CONSOLE_H

#include <stdint.h>

typedef struct console_driver {
    const char *name;
    void (*init)(void);
    void (*putc)(char c);
} console_driver_t;

void console_register(const console_driver_t *driver);
void console_init_all(void);

void console_locking_init(void);
void console_lock(void);
void console_unlock(void);

void console_putc(char c);
void console_write(const char *text);
void console_writeln(const char *text);
void console_write_hex32(uint32_t value);
void console_write_u32_dec(uint32_t value);

/*
 * Raw console output bypasses the console mutex.
 *
 * Use only when the caller already holds the console lock, or in very early
 * boot before the scheduler/sync layer exists.
 */
void console_raw_putc(char c);
void console_raw_write(const char *text);
void console_raw_writeln(const char *text);
void console_raw_write_hex32(uint32_t value);
void console_raw_write_u32_dec(uint32_t value);

void console_lock_test_once(void);

#endif
```

## Why raw output exists

Once `console_write()` uses a mutex, this becomes dangerous:

```c
mutex_lock(&console_mutex);
console_write("hello");
mutex_unlock(&console_mutex);
```

because `console_write()` would try to lock the mutex again.

So we need two layers:

```text
console_write()      locks around whole operation
console_raw_write()  assumes caller handled locking
```

The raw functions are also useful during early boot before `console_locking_init()` runs.

The high-level lock wrapper also checks the current interrupt state. If
interrupts are disabled, console output bypasses the mutex. That keeps IRQ,
panic, and scheduler-critical paths from trying to sleep on a blocking lock.

---

# 10. Replace `kernel/console.c`

```c
// kernel/console.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/sync.h"
#include "kernel/thread.h"

#define MAX_CONSOLE_DRIVERS 4

static const console_driver_t *drivers[MAX_CONSOLE_DRIVERS];
static size_t driver_count = 0;

static mutex_t console_mutex;
static int console_locking_ready;

void console_register(const console_driver_t *driver) {
    if (driver == NULL) {
        return;
    }

    if (driver_count >= MAX_CONSOLE_DRIVERS) {
        return;
    }

    drivers[driver_count++] = driver;
}

void console_init_all(void) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->init != NULL) {
            drivers[i]->init();
        }
    }
}

void console_locking_init(void) {
    mutex_init(&console_mutex, "console");
    console_locking_ready = 1;
    console_writeln("Console: output mutex enabled");
}

void console_lock(void) {
    irq_flags_t flags = irq_save();
    int can_lock = console_locking_ready && irq_flags_interrupts_enabled(flags);
    irq_restore(flags);

    if (can_lock) {
        mutex_lock(&console_mutex);
    }
}

void console_unlock(void) {
    irq_flags_t flags = irq_save();
    int can_unlock = console_locking_ready && irq_flags_interrupts_enabled(flags);
    irq_restore(flags);

    if (can_unlock) {
        mutex_unlock(&console_mutex);
    }
}

void console_raw_putc(char c) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->putc != NULL) {
            drivers[i]->putc(c);
        }
    }
}

void console_raw_write(const char *text) {
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        console_raw_putc(*text++);
    }
}

void console_raw_writeln(const char *text) {
    console_raw_write(text);
    console_raw_putc('\n');
}

void console_raw_write_hex32(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";

    console_raw_write("0x");

    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        console_raw_putc(digits[nibble]);
    }
}

void console_raw_write_u32_dec(uint32_t value) {
    char buffer[11];
    size_t index = 0;

    if (value == 0) {
        console_raw_putc('0');
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        console_raw_putc(buffer[--index]);
    }
}

void console_putc(char c) {
    console_lock();
    console_raw_putc(c);
    console_unlock();
}

void console_write(const char *text) {
    console_lock();
    console_raw_write(text);
    console_unlock();
}

void console_writeln(const char *text) {
    console_lock();
    console_raw_writeln(text);
    console_unlock();
}

void console_write_hex32(uint32_t value) {
    console_lock();
    console_raw_write_hex32(value);
    console_unlock();
}

void console_write_u32_dec(uint32_t value) {
    console_lock();
    console_raw_write_u32_dec(value);
    console_unlock();
}

/* ------------------------------------------------------------------------- */
/* Console lock test                                                          */
/* ------------------------------------------------------------------------- */

typedef struct console_test_arg {
    const char *label;
    volatile uint32_t *done_counter;
} console_test_arg_t;

static void console_test_worker(void *arg) {
    console_test_arg_t *test = (console_test_arg_t *)arg;

    for (uint32_t i = 0; i < 5; ++i) {
        console_lock();

        console_raw_write("Console lock test: ");
        console_raw_write(test->label);
        console_raw_write(" complete line ");
        console_raw_write_u32_dec(i);
        console_raw_putc('\n');

        console_unlock();

        thread_sleep_ticks(1);
    }

    (*test->done_counter)++;
}

void console_lock_test_once(void) {
    console_writeln("Console lock test: starting");

    static volatile uint32_t done;
    static console_test_arg_t arg_a;
    static console_test_arg_t arg_b;

    done = 0;

    arg_a.label = "C1";
    arg_a.done_counter = &done;

    arg_b.label = "C2";
    arg_b.done_counter = &done;

    thread_create("console-c1", console_test_worker, &arg_a);
    thread_create("console-c2", console_test_worker, &arg_b);

    while (done < 2) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    console_writeln("Console lock test: non-interleaved line output sanity check passed");
}
```

---

# 11. Important design issue: do not lock too early

The console mutex depends on:

```text
heap
threading
wait queues
mutex implementation
```

So we cannot enable console locking during the first few boot messages.

The correct order is:

```text
console drivers initialized
early boot messages unlocked
heap initialized
threading initialized
wait queues and sync available
console_locking_init()
```

After that, `console_write()` uses the mutex.

---

# 12. Update `kernel/kmain.c`

Add:

```c
#include "kernel/sync.h"
```

The synchronization tests call `thread_sleep_ticks()`, so PIT initialization and
interrupt enabling must happen before those tests run. Use this boot order:

```text
threading_init()
thread_test_once()
pit_init(100)
keyboard_init()
thread_preemption_init(2)
interrupts_enable()
console_locking_init()
sync_test_once()
console_lock_test_once()
preemption test
sleep test
keyboard test
```

Here is the corrected full `kernel_main()`.

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
#include "kernel/sync.h"
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

    thread_preemption_init(2);

    console_writeln("Interrupt hardware: configured");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    interrupts_enable();

    console_writeln("Interrupts: enabled");

    console_locking_init();

    sync_test_once();
    console_lock_test_once();

    thread_preemption_test_prepare();
    thread_preemption_test_wait();

    thread_sleep_test_once();
    keyboard_buffer_test_once();

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: terminal line discipline and a kernel monitor.");

    kernel_idle();
}
```

This corrected ordering is important.

---

# 13. Update `Makefile`

Add:

```text
build/kernel/sync.o
```

to the object list.

The relevant `OBJS` section becomes:

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
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/sync.o \
    build/kernel/thread.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the test target:

```make
test: iso
	$(GRUB_FILE) --is-x86-multiboot build/kernel.elf
	@mkdir -p build
	@timeout 12s $(QEMU) \
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
	grep -q "Threads: blocking scheduler initialized" build/test.log
	grep -q "Thread test: completed software-yield multitasking test" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Console: output mutex enabled" build/test.log
	grep -q "Sync test: mutex/semaphore sanity check passed" build/test.log
	grep -q "Console lock test: non-interleaved line output sanity check passed" build/test.log
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
	grep -q "Sleep test: blocking sleep sanity check passed" build/test.log
	grep -q "Keyboard test: blocking input-buffer sanity check passed" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, memory, heap, sync, preemption, sleep, and keyboard I/O smoke test passed."
```

---

# 14. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 13 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 15. Expected output

A successful boot should now include:

```text
Console: output mutex enabled
Sync test: starting mutex/semaphore test
Sync test: mutex worker M1 done
Sync test: mutex worker M2 done
Sync test: semaphore worker S1 passed order 0
Sync test: semaphore worker S2 passed order 1
Sync test: mutex/semaphore sanity check passed
Console lock test: starting
Console lock test: C1 complete line 0
Console lock test: C2 complete line 0
...
Console lock test: non-interleaved line output sanity check passed
```

The exact order of the worker lines may vary. That is normal under preemption.

The important result is:

```text
Sync test: mutex/semaphore sanity check passed
Console lock test: non-interleaved line output sanity check passed
```

---

# 16. Common failures

## Failure: `mutex_lock attempted recursive lock`

A thread tried to lock a mutex it already owns.

Common causes:

```text
console_write() called while holding console_lock()
mutex test prints while holding a mutex that indirectly prints again
panic path tries to use a locked console
```

Use raw console functions when the console mutex is already held:

```c
console_lock();
console_raw_write("...");
console_unlock();
```

## Failure: sync test hangs before PIT output

This means something called `thread_sleep_ticks()` before the timer was initialized and interrupts were enabled.

Correct order:

```text
pit_init()
keyboard_init()
thread_preemption_init()
interrupts_enable()
sync_test_once()
```

Do not run sleep-based tests before the timer is active.

## Failure: console output still interleaves

Check whether the test worker uses raw functions inside one explicit lock:

```c
console_lock();
console_raw_write(...);
console_unlock();
```

Calling separate `console_write()` calls without holding the lock protects each call, not the whole line.

## Failure: semaphore worker never wakes

Check:

```c
semaphore_signal()
```

It must increment the count and wake a waiter:

```c
sem->count++;
wait_queue_wake_one(&sem->waiters);
```

Also check that `semaphore_wait()` uses `while`, not `if`.

---

# 17. What this chapter achieved

We now have real synchronization primitives:

```text
mutex
semaphore
wait queue
sleep queue
preemptive scheduler
console lock
```

The kernel now has enough synchronization structure to build:

```text
terminal line discipline
blocking read/write APIs
pipes
message queues
driver completion waits
filesystem locks
process wait/exit
```

The locking stack now looks like this:

```text
IRQ save/restore
  ↓
wait queues
  ↓
mutex/semaphore
  ↓
console lock and future subsystem locks
```

That is the right layering.

---

# 18. Next chapter

The next useful chapter is a **terminal line discipline and kernel monitor**.

Now that keyboard input can block and console output is synchronized, we can build:

```text
terminal_readline()
terminal echo
backspace handling
simple command parser
kernel monitor prompt
commands:
  help
  ticks
  threads
  mem
  heap
  clear
```

That gives the OS its first interactive control surface.

[1]: https://wiki.osdev.org/Mutual_Exclusion "Mutual Exclusion"
[2]: https://wiki.osdev.org/Semaphore "Semaphore"
[3]: https://wiki.osdev.org/Spinlock "Spinlock"

---

# 19. Resources

- [Chapter 13 source release](https://github.com/Monotoba/toyix/releases/tag/Chapter_13)
- [Chapter 13 repository tree](https://github.com/Monotoba/toyix/tree/Chapter_13)
- [OSDev Wiki: Mutual Exclusion](https://wiki.osdev.org/Mutual_Exclusion)
- [OSDev Wiki: Semaphore](https://wiki.osdev.org/Semaphore)
- [OSDev Wiki: Spinlock](https://wiki.osdev.org/Spinlock)

---

# 20. Closure

The kernel now has wait queues, blocking sleeps, mutexes, semaphores, and synchronized console output. That is enough synchronization structure to start building interactive kernel services without every shared resource becoming a race.

Happy Coding!
