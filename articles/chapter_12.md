# Chapter 12 - Wait Queues and Blocking Keyboard Input

In Chapter 11, we added the first blocking primitive:

```c
thread_sleep_ticks(10);
```

That gave the scheduler a way to remove a thread from the ready queue until a future timer tick.

Now we need a more general primitive:

```text
sleep until some event happens
```

This is the foundation for real kernel I/O.

A keyboard reader should not spin like this:

```c
while (!keyboard_has_char()) {
    // waste CPU forever
}
```

It should block:

```text
reader thread waits
  ↓
keyboard IRQ receives character
  ↓
keyboard driver wakes reader
  ↓
reader continues
```

This chapter adds:

```text
wait queues
thread_block_current()
thread_wake()
keyboard input ring buffer
keyboard_getchar_blocking()
keyboard buffer test using synthetic input
```

This is the first device-style blocking I/O path in the kernel.

---

# 1. The concept: wait queues

A **wait queue** is a list of threads waiting for some condition.

Examples:

```text
keyboard input available
disk request completed
pipe has data
mutex unlocked
message received
child process exited
```

The general pattern is:

```text
thread checks condition
  ↓
condition is false
  ↓
thread joins wait queue
  ↓
thread blocks
  ↓
event occurs
  ↓
event producer wakes one or more waiters
```

For the keyboard:

```text
keyboard_getchar_blocking()
  ↓
ring buffer empty
  ↓
current thread sleeps on keyboard wait queue
  ↓
keyboard IRQ receives scancode
  ↓
driver converts it to ASCII
  ↓
driver pushes character into ring buffer
  ↓
driver wakes one reader
```

---

# 2. Why this must be race-safe

The dangerous race looks like this:

```text
reader checks buffer: empty
keyboard IRQ receives character
keyboard wakes nobody because reader is not asleep yet
reader goes to sleep
reader sleeps forever even though data exists
```

To prevent that, the condition check and queue insertion must happen while interrupts are disabled.

The core shape is:

```c
irq_flags_t flags = irq_save();

while (!condition()) {
    add current thread to wait queue;
    mark current thread BLOCKED;
    software-yield into scheduler while interrupts are still disabled;
}

irq_restore(flags);
```

Calling the scheduler while interrupts are disabled is intentional here. The blocked thread's saved frame will resume with interrupts still disabled, then the wait function restores the original interrupt state before returning to normal code.

---

# 3. Patch overview

Add:

```text
include/kernel/
└── wait_queue.h

kernel/
└── wait_queue.c
```

Modify:

```text
include/kernel/thread.h
kernel/thread.c
drivers/input/keyboard.h
drivers/input/keyboard.c
kernel/kmain.c
Makefile
tests/smoke.sh
```

No new assembly is needed.

---

# 4. Update `include/kernel/thread.h`

Add these declarations:

```c
void thread_block_current(void);
void thread_wake(thread_t *thread);
```

Here is the full updated header for clarity.

```c
// include/kernel/thread.h
#ifndef TOYIX_KERNEL_THREAD_H
#define TOYIX_KERNEL_THREAD_H

#include <stdint.h>
#include "arch/x86/interrupts.h"

#define THREAD_STACK_SIZE 16384u

typedef void (*thread_entry_t)(void *arg);

typedef enum thread_state {
    THREAD_NEW = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_ZOMBIE
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

    uint32_t wake_tick;

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

void thread_block_current(void);
void thread_wake(thread_t *thread);

void thread_sleep_ticks(uint32_t ticks);

void thread_on_timer_tick(interrupt_frame_t *frame);
int thread_should_reschedule(void);
uintptr_t thread_schedule_from_interrupt(interrupt_frame_t *frame);
uintptr_t schedule_interrupt_handler(interrupt_frame_t *frame);

void thread_preemption_init(uint32_t ticks_per_slice);
void thread_preemption_test_prepare(void);
void thread_preemption_test_wait(void);

void thread_reap_zombies(void);

uint32_t thread_ready_count(void);
uint32_t thread_sleeping_count(void);
uint32_t thread_zombie_count(void);
uint32_t thread_ticks(void);

void thread_dump_state(void);
void thread_test_once(void);
void thread_sleep_test_once(void);

#endif
```

---

# 5. Update `kernel/thread.c`

Add these two functions after `thread_yield()`.

```c
void thread_block_current(void) {
    irq_flags_t flags = irq_save();

    thread_t *self = current_thread;
    validate_thread(self);

    if (self == idle_thread) {
        irq_restore(flags);
        kernel_panic("idle thread attempted to block");
    }

    if (self->state != THREAD_RUNNING) {
        irq_restore(flags);
        kernel_panic("thread_block_current called on non-running thread");
    }

    self->state = THREAD_BLOCKED;
    reschedule_requested = 1;

    irq_restore(flags);
}

void thread_wake(thread_t *thread) {
    if (thread == 0) {
        return;
    }

    irq_flags_t flags = irq_save();

    validate_thread(thread);

    if (thread->state == THREAD_BLOCKED) {
        ready_push(thread);
        reschedule_requested = 1;
    }

    irq_restore(flags);
}
```

## Why these functions are small

They do not know why a thread is blocked.

That is deliberate.

A thread may be blocked because it is waiting for:

```text
keyboard input
disk I/O
a mutex
a semaphore
a pipe
a message queue
```

The scheduler only cares that the thread is not runnable until someone wakes it.

---

# 6. Add `include/kernel/wait_queue.h`

```c
// include/kernel/wait_queue.h
#ifndef TOYIX_KERNEL_WAIT_QUEUE_H
#define TOYIX_KERNEL_WAIT_QUEUE_H

#include <stdint.h>
#include "kernel/thread.h"

typedef int (*wait_condition_t)(void *context);

typedef struct wait_queue {
    const char *name;
    thread_t *head;
    thread_t *tail;
} wait_queue_t;

void wait_queue_init(wait_queue_t *queue, const char *name);

void wait_queue_wait(
    wait_queue_t *queue,
    wait_condition_t condition,
    void *context
);

void wait_queue_wake_one(wait_queue_t *queue);
void wait_queue_wake_all(wait_queue_t *queue);

uint32_t wait_queue_count(wait_queue_t *queue);

#endif
```

## Why `wait_queue_wait()` takes a condition

The condition callback prevents the lost-wakeup race.

The wait queue function repeatedly checks the condition while interrupts are disabled. It only blocks the thread if the condition is still false.

That way, if the event already happened before the thread sleeps, the thread does not sleep.

---

# 7. Add `kernel/wait_queue.c`

```c
// kernel/wait_queue.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "kernel/panic.h"
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

static void wait_queue_push_locked(wait_queue_t *queue, thread_t *thread) {
    if (queue == 0 || thread == 0) {
        kernel_panic("wait_queue_push_locked received null pointer");
    }

    thread->next = 0;
    thread->prev = queue->tail;

    if (queue->tail != 0) {
        queue->tail->next = thread;
    } else {
        queue->head = thread;
    }

    queue->tail = thread;
}

static thread_t *wait_queue_pop_locked(wait_queue_t *queue) {
    if (queue == 0) {
        kernel_panic("wait_queue_pop_locked received null queue");
    }

    thread_t *thread = queue->head;

    if (thread == 0) {
        return 0;
    }

    queue->head = thread->next;

    if (queue->head != 0) {
        queue->head->prev = 0;
    } else {
        queue->tail = 0;
    }

    thread->next = 0;
    thread->prev = 0;

    return thread;
}

void wait_queue_init(wait_queue_t *queue, const char *name) {
    if (queue == 0) {
        kernel_panic("wait_queue_init received null queue");
    }

    queue->name = name != 0 ? name : "unnamed";
    queue->head = 0;
    queue->tail = 0;
}

void wait_queue_wait(
    wait_queue_t *queue,
    wait_condition_t condition,
    void *context
) {
    if (queue == 0 || condition == 0) {
        kernel_panic("wait_queue_wait received null argument");
    }

    irq_flags_t flags = irq_save();

    while (!condition(context)) {
        thread_t *self = thread_current();

        wait_queue_push_locked(queue, self);
        thread_block_current();

        /*
         * The thread resumes here through the scheduler with interrupts still
         * disabled. That makes it safe to re-check the condition before
         * restoring the caller's original interrupt state.
         */
        thread_yield();
    }

    irq_restore(flags);
}

void wait_queue_wake_one(wait_queue_t *queue) {
    if (queue == 0) {
        return;
    }

    irq_flags_t flags = irq_save();
    thread_t *thread = wait_queue_pop_locked(queue);
    irq_restore(flags);

    if (thread != 0) {
        thread_wake(thread);
    }
}

void wait_queue_wake_all(wait_queue_t *queue) {
    if (queue == 0) {
        return;
    }

    for (;;) {
        irq_flags_t flags = irq_save();
        thread_t *thread = wait_queue_pop_locked(queue);
        irq_restore(flags);

        if (thread == 0) {
            return;
        }

        thread_wake(thread);
    }
}

uint32_t wait_queue_count(wait_queue_t *queue) {
    if (queue == 0) {
        return 0;
    }

    irq_flags_t flags = irq_save();
    uint32_t count = 0;

    for (thread_t *thread = queue->head; thread != 0; thread = thread->next) {
        count++;
    }

    irq_restore(flags);

    return count;
}
```

---

# 8. Update `drivers/input/keyboard.h`

Replace the old header with this version.

```c
// drivers/input/keyboard.h
#ifndef TOYIX_DRIVERS_INPUT_KEYBOARD_H
#define TOYIX_DRIVERS_INPUT_KEYBOARD_H

int keyboard_init(void);

int keyboard_try_getchar(char *out);
char keyboard_getchar_blocking(void);

void keyboard_debug_inject_char(char ch);
void keyboard_buffer_test_once(void);

#endif
```

## What changed?

The keyboard is no longer just an IRQ echo demo.

It now exposes a small input API:

```c
keyboard_try_getchar()
keyboard_getchar_blocking()
```

The blocking version is the important one.

---

# 9. Replace `drivers/input/keyboard.c`

```c
// drivers/input/keyboard.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/irq_state.h"
#include "arch/x86/pic.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

#define KEYBOARD_DATA_PORT 0x60u
#define KEYBOARD_RING_SIZE 128u

static wait_queue_t keyboard_wait_queue;

static char keyboard_ring[KEYBOARD_RING_SIZE];
static uint32_t keyboard_head;
static uint32_t keyboard_tail;
static uint32_t keyboard_count;

static volatile uint32_t keyboard_test_done;
static char keyboard_test_result[4];

static const char scancode_set1_ascii[128] = {
    [0x01] = 0,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',

    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',

    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',

    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' '
};

static int keyboard_has_data(void *context) {
    (void)context;
    return keyboard_count > 0;
}

static int keyboard_ring_push(char ch) {
    if (keyboard_count >= KEYBOARD_RING_SIZE) {
        return 0;
    }

    keyboard_ring[keyboard_tail] = ch;
    keyboard_tail = (keyboard_tail + 1u) % KEYBOARD_RING_SIZE;
    keyboard_count++;

    return 1;
}

static int keyboard_ring_pop(char *out) {
    if (keyboard_count == 0 || out == 0) {
        return 0;
    }

    *out = keyboard_ring[keyboard_head];
    keyboard_head = (keyboard_head + 1u) % KEYBOARD_RING_SIZE;
    keyboard_count--;

    return 1;
}

static void keyboard_deliver_char(char ch) {
    irq_flags_t flags = irq_save();
    int pushed = keyboard_ring_push(ch);
    irq_restore(flags);

    if (pushed) {
        wait_queue_wake_one(&keyboard_wait_queue);
    }
}

static void keyboard_irq_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    /*
     * In set 1, the high bit usually marks a key release.
     * For this first driver, we ignore release and only queue presses.
     */
    if ((scancode & 0x80u) != 0) {
        return;
    }

    if (scancode < 128) {
        char ch = scancode_set1_ascii[scancode];

        if (ch != 0) {
            keyboard_deliver_char(ch);
        }
    }
}

int keyboard_init(void) {
    keyboard_head = 0;
    keyboard_tail = 0;
    keyboard_count = 0;
    keyboard_test_done = 0;
    keyboard_test_result[0] = 0;
    keyboard_test_result[1] = 0;
    keyboard_test_result[2] = 0;
    keyboard_test_result[3] = 0;

    wait_queue_init(&keyboard_wait_queue, "keyboard");

    interrupt_register_handler(33, keyboard_irq_handler);
    pic_clear_mask(1);

    console_writeln("Keyboard: IRQ1 handler and input buffer installed");
    return 0;
}

int keyboard_try_getchar(char *out) {
    if (out == 0) {
        return 0;
    }

    irq_flags_t flags = irq_save();
    int ok = keyboard_ring_pop(out);
    irq_restore(flags);

    return ok;
}

char keyboard_getchar_blocking(void) {
    char ch;

    for (;;) {
        wait_queue_wait(&keyboard_wait_queue, keyboard_has_data, 0);

        irq_flags_t flags = irq_save();
        int ok = keyboard_ring_pop(&ch);
        irq_restore(flags);

        if (ok) {
            return ch;
        }
    }
}

void keyboard_debug_inject_char(char ch) {
    keyboard_deliver_char(ch);
}

static void keyboard_reader_test_thread(void *arg) {
    (void)arg;

    keyboard_test_result[0] = keyboard_getchar_blocking();
    keyboard_test_result[1] = keyboard_getchar_blocking();
    keyboard_test_result[2] = keyboard_getchar_blocking();
    keyboard_test_result[3] = '\0';

    console_write("Keyboard test: reader received ");
    console_writeln(keyboard_test_result);

    keyboard_test_done = 1;
}

void keyboard_buffer_test_once(void) {
    console_writeln("Keyboard test: starting blocking input-buffer test");

    keyboard_test_done = 0;
    keyboard_test_result[0] = 0;
    keyboard_test_result[1] = 0;
    keyboard_test_result[2] = 0;
    keyboard_test_result[3] = 0;

    thread_create("kbd-reader", keyboard_reader_test_thread, 0);

    /*
     * Let the reader run and block on the empty keyboard queue.
     */
    thread_sleep_ticks(2);

    keyboard_debug_inject_char('o');
    thread_sleep_ticks(1);

    keyboard_debug_inject_char('s');
    thread_sleep_ticks(1);

    keyboard_debug_inject_char('!');
    thread_sleep_ticks(1);

    while (!keyboard_test_done) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (keyboard_test_result[0] != 'o' ||
        keyboard_test_result[1] != 's' ||
        keyboard_test_result[2] != '!') {
        kernel_panic("keyboard blocking buffer test failed");
    }

    console_writeln("Keyboard test: blocking input-buffer sanity check passed");
}
```

---

# 10. Why the keyboard IRQ no longer prints directly

Earlier, the keyboard interrupt handler called:

```c
console_putc(ch);
```

That was useful for proving IRQ1 worked, but it is not a good driver design.

An interrupt handler should do the minimum practical work:

```text
read hardware
store event/data
wake waiter
return
```

The consumer of the input should decide what to do with the character.

That gives us a better architecture:

```text
keyboard IRQ
  ↓
keyboard ring buffer
  ↓
wait queue wakeup
  ↓
reader thread
  ↓
console echo, shell, editor, etc.
```

Later, a terminal driver can read from the keyboard buffer and implement line editing.

---

# 11. Update `kernel/kmain.c`

Add the keyboard buffer test after the sleep test.

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

    thread_preemption_init(2);
    thread_preemption_test_prepare();

    console_writeln("Interrupt hardware: configured");

#ifdef TOYIX_TRIGGER_TEST_EXCEPTION
    console_writeln("Triggering test exception with UD2...");
    __asm__ volatile ("ud2");
#endif

    interrupts_enable();

    console_writeln("Interrupts: enabled");

    thread_preemption_test_wait();
    thread_sleep_test_once();
    keyboard_buffer_test_once();

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: locks, wait queues, and keyboard input buffering.");

    kernel_idle();
}
```

---

# 12. Update `Makefile`

Add:

```text
build/kernel/wait_queue.o
```

to the object list.

The relevant object section becomes:

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
    build/arch/x86/vmm.o \
    build/kernel/kmain.o \
    build/kernel/idle.o \
    build/kernel/console.o \
    build/kernel/heap.o \
    build/kernel/panic.o \
    build/kernel/pmm.o \
    build/kernel/thread.o \
    build/kernel/vmem.o \
    build/kernel/wait_queue.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the test greps:

```make
test: iso
	@mkdir -p build
	@rm -f build/test.log
	@timeout 10s $(QEMU) \
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
	grep -q "Heap: initialized virtual heap with 4 page(s)" build/test.log
	grep -q "Heap test: VMM-backed allocation/free sanity check passed" build/test.log
	grep -q "Threads: blocking scheduler initialized" build/test.log
	grep -q "Thread test: completed software-yield multitasking test" build/test.log
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
	grep -q "Sleep test: blocking sleep sanity check passed" build/test.log
	grep -q "Keyboard: IRQ1 handler and input buffer installed" build/test.log
	grep -q "Keyboard test: blocking input-buffer sanity check passed" build/test.log
	grep -q "Interrupts: enabled" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	grep -q "VMM: initialized kernel address-space mapper" build/test.log
	grep -q "VMM test: map/translate/write/unmap sanity check passed" build/test.log
	@echo "Boot, memory, heap, preemption, sleep, and keyboard blocking I/O smoke test passed."
```

---

# 13. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 12 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 14. Expected output

You should now see lines like:

```text
Keyboard: IRQ1 handler and input buffer installed
Keyboard test: starting blocking input-buffer test
Keyboard test: reader received os!
Keyboard test: blocking input-buffer sanity check passed
Timer: observed 3 ticks
```

This proves:

```text
reader thread blocked on keyboard wait queue
synthetic keyboard input entered ring buffer
keyboard wait queue woke reader
reader consumed characters
reader exited
zombie reaper cleaned it up
```

That is a real blocking I/O pattern.

---

# 15. Important limitation

The keyboard driver is still primitive.

It does not yet handle:

```text
Shift
Ctrl
Alt
Caps Lock
extended scancodes
keyboard LEDs
non-US layouts
line editing
terminal canonical mode
```

That is acceptable. The goal of this chapter is not a perfect keyboard driver.

The goal is:

```text
IRQ producer wakes blocked consumer
```

That is now working.

---

# 16. Common failures

## Failure: reader sleeps forever

Likely causes:

```text
keyboard_debug_inject_char() did not call wait_queue_wake_one()
wait_queue_wait() did not mark thread BLOCKED
scheduler requeued BLOCKED thread incorrectly
thread_wake() did not move BLOCKED thread to ready queue
```

Check these lines:

```c
wait_queue_push_locked(queue, self);
thread_block_current();
thread_yield();
```

and:

```c
if (thread->state == THREAD_BLOCKED) {
    ready_push(thread);
}
```

## Failure: wait queue corrupts ready queue

A blocked thread uses the same `next` and `prev` fields for the wait queue that it used for the ready queue.

That is only safe because a thread must not be in two queues at once.

The correct state transition is:

```text
RUNNING
  ↓ wait_queue_wait
BLOCKED and on wait queue
  ↓ wait_queue_wake_one
READY and on ready queue
```

If a thread is accidentally inserted into the wait queue while still on the ready queue, the linked lists will corrupt.

## Failure: keyboard test sometimes passes, sometimes hangs

That usually means a lost-wakeup race.

The condition check must happen inside `wait_queue_wait()` while interrupts are disabled:

```c
while (!condition(context)) {
    add to wait queue
    block
    yield
}
```

Do not write blocking code as:

```c
if (!condition()) {
    wait_queue_wait_without_rechecking();
}
```

That loses wakeups.

---

# 17. What this chapter achieved

We now have a general blocking mechanism:

```text
wait queue
  ↓
block current thread
  ↓
event wakes thread
  ↓
scheduler runs it later
```

And the first device-style client:

```text
keyboard IRQ
  ↓
ring buffer
  ↓
wait queue wakeup
  ↓
blocking reader
```

---

# Resources

- [Chapter 12 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_12)
- [OSDev blocking process](https://wiki.osdev.org/Blocking_Process)
- [OSDev keyboard controller](https://wiki.osdev.org/Keyboard_Controller)
- [OSDev context switching](https://wiki.osdev.org/Context_Switching)

# Closure

Chapter 12 turns the blocking primitives from a sleep-only demonstration into a real event-driven handoff. Threads can now wait on a queue, the keyboard driver can store input instead of printing directly, and a reader can block until a producer wakes it.

That gives us the kernel-side pattern we need for mutexes, semaphores, and eventually terminal input that blocks cleanly instead of spinning.

Happy Coding!
