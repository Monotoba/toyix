# Chapter 11 — Blocking Primitives, Sleep Queues, and Scheduler Hygiene

In Chapter 10, we added timer-driven preemption:

```text
PIT IRQ0
  ↓
scheduler tick
  ↓
time slice expires
  ↓
switch to another thread
```

Now we need the next scheduler capability: **blocking**.

A runnable thread competes for CPU time. A blocked or sleeping thread does not. OSDev describes a blocking process as one that waits for an event, such as a semaphore or message, and is removed from the active scheduling queue until that event occurs. ([OSDev Wiki][1])

This chapter adds:

```text
interrupt-save/restore critical sections
idle thread
sleep queue
thread_sleep_ticks()
zombie queue
thread reaper
safe ready/sleep/zombie queue manipulation
blocking test
```

The key new primitive will be:

```c
thread_sleep_ticks(10);
```

That means:

```text
current thread stops running
  ↓
thread is placed on sleep queue
  ↓
timer ticks continue
  ↓
scheduler wakes thread later
  ↓
thread returns to ready queue
```

---

# 1. Why this chapter matters

Preemption creates a new kind of bug.

Before preemption, this was mostly safe:

```c
ready_push(thread);
```

After preemption, IRQ0 can interrupt almost anywhere. If the timer interrupt fires while the kernel is halfway through modifying the ready queue, the scheduler may inspect a corrupted queue.

So we need short critical sections:

```c
irq_flags_t flags = irq_save();
/* modify scheduler queues */
irq_restore(flags);
```

The important detail is that we save the old interrupt state, disable interrupts, do the small critical update, then restore the previous state. Brendan’s OSDev multitasking tutorial specifically warns that in higher-level language code you cannot just leave flags on the stack; you need to save the old flags value and restore it later. ([OSDev Wiki][2])

This chapter stays single-CPU. We are not building SMP spinlocks yet. On one CPU, disabling interrupts is enough to prevent timer preemption while touching scheduler queues. OSDev discussions often recommend disabling interrupts around scheduler-critical sections to prevent the scheduler or interrupt handlers from reentering sensitive code. ([OSDev][3])

---

# 2. Add `arch/x86/irq_state.h`

```c
// arch/x86/irq_state.h
#ifndef TOYIX_ARCH_X86_IRQ_STATE_H
#define TOYIX_ARCH_X86_IRQ_STATE_H

#include <stdint.h>

typedef uint32_t irq_flags_t;

static inline irq_flags_t irq_save(void) {
    irq_flags_t flags;

    __asm__ volatile (
        "pushfl\n"
        "popl %0\n"
        "cli\n"
        : "=r"(flags)
        :
        : "memory"
    );

    return flags;
}

static inline void irq_restore(irq_flags_t flags) {
    __asm__ volatile (
        "pushl %0\n"
        "popfl\n"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

static inline int irq_flags_interrupts_enabled(irq_flags_t flags) {
    return (flags & 0x00000200u) != 0;
}

#endif
```

## What this does

`irq_save()` saves `EFLAGS`, then disables maskable interrupts with `cli`.

`irq_restore(flags)` restores the earlier flags with `popfl`.

That means this works correctly even if interrupts were already disabled before entering the critical section:

```c
irq_flags_t flags = irq_save();
/* critical section */
irq_restore(flags);
```

If interrupts were enabled, they become enabled again.

If interrupts were disabled, they stay disabled.

That distinction matters.

---

# 3. Replace `include/kernel/thread.h`

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

## What changed?

We added real blocked states:

```c
THREAD_BLOCKED,
THREAD_SLEEPING,
THREAD_ZOMBIE
```

For this chapter:

| State      | Meaning                                         |
| ---------- | ----------------------------------------------- |
| `READY`    | Can run when scheduled                          |
| `RUNNING`  | Currently executing                             |
| `SLEEPING` | Waiting for a future timer tick                 |
| `BLOCKED`  | Reserved for future waits, locks, messages, I/O |
| `ZOMBIE`   | Finished, but not yet safely freed              |

The `wake_tick` field is used by the sleep queue.

---

# 4. Replace `kernel/thread.c`

This is the main scheduler upgrade.

```c
// kernel/thread.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "arch/x86/interrupts.h"
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/thread.h"

#define THREAD_MAGIC 0x54485244u
#define INITIAL_EFLAGS 0x00000202u

static thread_t bootstrap_thread;
static thread_t *idle_thread;

static thread_t *current_thread;

static thread_t *ready_head;
static thread_t *ready_tail;

static thread_t *sleep_head;
static thread_t *sleep_tail;

static thread_t *zombie_head;
static thread_t *zombie_tail;

static uint32_t next_thread_id;
static uint32_t zombie_created_count;
static uint32_t zombie_reaped_count;

static int scheduler_initialized;
static int preemption_enabled;

static uint32_t ticks_per_slice;
static uint32_t current_slice_ticks;
static int reschedule_requested;

static volatile uint32_t scheduler_ticks;
static volatile uint32_t preempt_test_done_count;
static volatile uint32_t sleep_test_done_count;

static void thread_bootstrap(void);
static void idle_thread_entry(void *arg);

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

        case THREAD_BLOCKED:
            return "BLOCKED";

        case THREAD_SLEEPING:
            return "SLEEPING";

        case THREAD_ZOMBIE:
            return "ZOMBIE";

        default:
            return "UNKNOWN";
    }
}

static void queue_push(
    thread_t **head,
    thread_t **tail,
    thread_t *thread
) {
    validate_thread(thread);

    thread->next = 0;
    thread->prev = *tail;

    if (*tail != 0) {
        (*tail)->next = thread;
    } else {
        *head = thread;
    }

    *tail = thread;
}

static thread_t *queue_pop(
    thread_t **head,
    thread_t **tail
) {
    thread_t *thread = *head;

    if (thread == 0) {
        return 0;
    }

    *head = thread->next;

    if (*head != 0) {
        (*head)->prev = 0;
    } else {
        *tail = 0;
    }

    thread->next = 0;
    thread->prev = 0;

    return thread;
}

static void ready_push(thread_t *thread) {
    if (thread == idle_thread) {
        return;
    }

    thread->state = THREAD_READY;
    queue_push(&ready_head, &ready_tail, thread);
}

static thread_t *ready_pop(void) {
    return queue_pop(&ready_head, &ready_tail);
}

static void zombie_push(thread_t *thread) {
    if (thread == idle_thread || thread == &bootstrap_thread) {
        return;
    }

    queue_push(&zombie_head, &zombie_tail, thread);
}

static thread_t *zombie_pop(void) {
    return queue_pop(&zombie_head, &zombie_tail);
}

static int tick_reached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
}

static void sleep_insert(thread_t *thread) {
    validate_thread(thread);

    thread->state = THREAD_SLEEPING;
    thread->next = 0;
    thread->prev = 0;

    if (sleep_head == 0) {
        sleep_head = thread;
        sleep_tail = thread;
        return;
    }

    thread_t *current = sleep_head;

    while (current != 0 &&
           !tick_reached(current->wake_tick, thread->wake_tick)) {
        current = current->next;
    }

    if (current == sleep_head) {
        thread->next = sleep_head;
        sleep_head->prev = thread;
        sleep_head = thread;
        return;
    }

    if (current == 0) {
        thread->prev = sleep_tail;
        sleep_tail->next = thread;
        sleep_tail = thread;
        return;
    }

    thread_t *previous = current->prev;

    previous->next = thread;
    thread->prev = previous;
    thread->next = current;
    current->prev = thread;
}

static thread_t *sleep_pop_front(void) {
    return queue_pop(&sleep_head, &sleep_tail);
}

static void wake_due_sleepers(void) {
    while (sleep_head != 0 &&
           tick_reached((uint32_t)scheduler_ticks, sleep_head->wake_tick)) {
        thread_t *thread = sleep_pop_front();

        validate_thread(thread);

        console_write("Threads: waking ");
        console_write(thread->name);
        console_write(" at tick ");
        console_write_u32_dec((uint32_t)scheduler_ticks);
        console_putc('\n');

        ready_push(thread);
        reschedule_requested = 1;
    }
}

uint32_t thread_ready_count(void) {
    irq_flags_t flags = irq_save();

    uint32_t count = 0;

    for (thread_t *t = ready_head; t != 0; t = t->next) {
        validate_thread(t);
        count++;
    }

    irq_restore(flags);
    return count;
}

uint32_t thread_sleeping_count(void) {
    irq_flags_t flags = irq_save();

    uint32_t count = 0;

    for (thread_t *t = sleep_head; t != 0; t = t->next) {
        validate_thread(t);
        count++;
    }

    irq_restore(flags);
    return count;
}

uint32_t thread_zombie_count(void) {
    irq_flags_t flags = irq_save();

    uint32_t count = 0;

    for (thread_t *t = zombie_head; t != 0; t = t->next) {
        validate_thread(t);
        count++;
    }

    irq_restore(flags);
    return count;
}

uint32_t thread_ticks(void) {
    return (uint32_t)scheduler_ticks;
}

static void thread_make_initial_stack(thread_t *thread) {
    validate_thread(thread);

    uintptr_t stack_top =
        (uintptr_t)thread->stack_base + thread->stack_size;

    stack_top = align_down(stack_top, 16u);

    uint32_t *sp = (uint32_t *)stack_top;

    /*
     * Build the stack frame restored by irq.asm and sched_interrupt.asm:
     *
     *   pop saved DS
     *   popa
     *   add esp, 8
     *   iretd
     */

    *(--sp) = INITIAL_EFLAGS;
    *(--sp) = X86_KERNEL_CODE_SELECTOR;
    *(--sp) = (uint32_t)thread_bootstrap;

    *(--sp) = 0;
    *(--sp) = X86_SCHED_INTERRUPT_VECTOR;

    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    *(--sp) = X86_KERNEL_DATA_SELECTOR;

    thread->context.esp = (uint32_t)sp;
}

static thread_t *thread_create_internal(
    const char *name,
    thread_entry_t entry,
    void *arg,
    int enqueue
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

    thread->wake_tick = 0;
    thread->next = 0;
    thread->prev = 0;

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
}

void threading_init(void) {
    ready_head = 0;
    ready_tail = 0;

    sleep_head = 0;
    sleep_tail = 0;

    zombie_head = 0;
    zombie_tail = 0;

    next_thread_id = 1;
    zombie_created_count = 0;
    zombie_reaped_count = 0;

    scheduler_initialized = 0;
    preemption_enabled = 0;

    ticks_per_slice = 1;
    current_slice_ticks = 0;
    reschedule_requested = 0;

    scheduler_ticks = 0;
    preempt_test_done_count = 0;
    sleep_test_done_count = 0;

    bootstrap_thread.magic = THREAD_MAGIC;
    bootstrap_thread.id = 0;
    bootstrap_thread.name = "bootstrap";
    bootstrap_thread.state = THREAD_RUNNING;
    bootstrap_thread.context.esp = 0;
    bootstrap_thread.stack_base = 0;
    bootstrap_thread.stack_size = 0;
    bootstrap_thread.entry = 0;
    bootstrap_thread.arg = 0;
    bootstrap_thread.wake_tick = 0;
    bootstrap_thread.next = 0;
    bootstrap_thread.prev = 0;

    current_thread = &bootstrap_thread;

    idle_thread = thread_create_internal(
        "idle",
        idle_thread_entry,
        0,
        0
    );

    scheduler_initialized = 1;

    console_writeln("Threads: blocking scheduler initialized");
}

thread_t *thread_create(
    const char *name,
    thread_entry_t entry,
    void *arg
) {
    return thread_create_internal(name, entry, arg, 1);
}

thread_t *thread_current(void) {
    return current_thread;
}

void thread_yield(void) {
    __asm__ volatile ("int $0x30");
}

void thread_sleep_ticks(uint32_t ticks) {
    if (ticks == 0) {
        thread_yield();
        return;
    }

    irq_flags_t flags = irq_save();

    if (!irq_flags_interrupts_enabled(flags)) {
        irq_restore(flags);
        kernel_panic("thread_sleep_ticks called with interrupts disabled");
    }

    thread_t *self = current_thread;
    validate_thread(self);

    if (self == idle_thread) {
        irq_restore(flags);
        return;
    }

    self->wake_tick = (uint32_t)scheduler_ticks + ticks;
    self->state = THREAD_SLEEPING;
    sleep_insert(self);
    reschedule_requested = 1;

    irq_restore(flags);

    /*
     * If a timer interrupt preempts us immediately after irq_restore(), this
     * thread may already be asleep when it resumes here. Calling yield is
     * still safe: the scheduler will not requeue a SLEEPING thread.
     */
    thread_yield();
}

void thread_exit(void) {
    irq_flags_t flags = irq_save();

    thread_t *old_thread = current_thread;
    validate_thread(old_thread);

    console_write("Thread: exiting ");
    console_write(old_thread->name);
    console_write(" id=");
    console_write_u32_dec(old_thread->id);
    console_putc('\n');

    old_thread->state = THREAD_ZOMBIE;
    zombie_created_count++;

    zombie_push(old_thread);
    reschedule_requested = 1;

    irq_restore(flags);

    thread_yield();

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

static void idle_thread_entry(void *arg) {
    (void)arg;

    for (;;) {
        interrupts_wait();
    }
}

uintptr_t thread_schedule_from_interrupt(interrupt_frame_t *frame) {
    if (frame == 0) {
        kernel_panic("scheduler received null interrupt frame");
    }

    if (!scheduler_initialized) {
        return 0;
    }

    thread_t *old_thread = current_thread;
    validate_thread(old_thread);

    /*
     * Save the interrupted/restorable frame for the current thread.
     */
    old_thread->context.esp = (uint32_t)(uintptr_t)frame;

    /*
     * If the old thread is still running and has competition, place it at the
     * end of the ready queue. Sleeping, blocked, and zombie threads are not
     * requeued.
     */
    if (old_thread != idle_thread && old_thread->state == THREAD_RUNNING) {
        if (ready_head == 0 && !reschedule_requested) {
            current_slice_ticks = 0;
            return 0;
        }

        ready_push(old_thread);
    }

    thread_t *next_thread = ready_pop();

    if (next_thread == 0) {
        if (old_thread == idle_thread &&
            old_thread->state == THREAD_RUNNING) {
            current_slice_ticks = 0;
            reschedule_requested = 0;
            return 0;
        }

        next_thread = idle_thread;
    }

    validate_thread(next_thread);

    next_thread->state = THREAD_RUNNING;
    current_thread = next_thread;

    current_slice_ticks = 0;
    reschedule_requested = 0;

    return (uintptr_t)next_thread->context.esp;
}

uintptr_t schedule_interrupt_handler(interrupt_frame_t *frame) {
    return thread_schedule_from_interrupt(frame);
}

void thread_on_timer_tick(interrupt_frame_t *frame) {
    (void)frame;

    if (!scheduler_initialized) {
        return;
    }

    scheduler_ticks++;

    wake_due_sleepers();

    if (!preemption_enabled) {
        return;
    }

    if (ready_head == 0) {
        current_slice_ticks = 0;
        return;
    }

    current_slice_ticks++;

    if (current_slice_ticks >= ticks_per_slice) {
        reschedule_requested = 1;
    }
}

int thread_should_reschedule(void) {
    return reschedule_requested;
}

void thread_preemption_init(uint32_t requested_ticks_per_slice) {
    if (requested_ticks_per_slice == 0) {
        requested_ticks_per_slice = 1;
    }

    irq_flags_t flags = irq_save();

    ticks_per_slice = requested_ticks_per_slice;
    current_slice_ticks = 0;
    reschedule_requested = 0;
    preemption_enabled = 1;

    irq_restore(flags);

    console_write("Threads: preemption enabled, slice ticks=");
    console_write_u32_dec(ticks_per_slice);
    console_putc('\n');
}

void thread_reap_zombies(void) {
    for (;;) {
        irq_flags_t flags = irq_save();
        thread_t *zombie = zombie_pop();
        irq_restore(flags);

        if (zombie == 0) {
            return;
        }

        validate_thread(zombie);

        console_write("Threads: reaping zombie ");
        console_write(zombie->name);
        console_write(" id=");
        console_write_u32_dec(zombie->id);
        console_putc('\n');

        void *stack = zombie->stack_base;

        zombie->magic = 0;

        if (stack != 0) {
            kfree(stack);
        }

        kfree(zombie);
        zombie_reaped_count++;
    }
}

void thread_dump_state(void) {
    irq_flags_t flags = irq_save();

    console_write("Threads: current=");
    console_write(current_thread != 0 ? current_thread->name : "(null)");
    console_write(" ready=");
    console_write_u32_dec(thread_ready_count());
    console_write(" sleeping=");
    console_write_u32_dec(thread_sleeping_count());
    console_write(" zombies=");
    console_write_u32_dec(thread_zombie_count());
    console_write(" ticks=");
    console_write_u32_dec((uint32_t)scheduler_ticks);
    console_putc('\n');

    console_write("Threads: zombies created=");
    console_write_u32_dec(zombie_created_count);
    console_write(" reaped=");
    console_write_u32_dec(zombie_reaped_count);
    console_putc('\n');

    for (thread_t *t = ready_head; t != 0; t = t->next) {
        validate_thread(t);

        console_write("  ready id=");
        console_write_u32_dec(t->id);
        console_write(" name=");
        console_write(t->name);
        console_write(" state=");
        console_write(thread_state_name(t->state));
        console_putc('\n');
    }

    for (thread_t *t = sleep_head; t != 0; t = t->next) {
        validate_thread(t);

        console_write("  sleeping id=");
        console_write_u32_dec(t->id);
        console_write(" name=");
        console_write(t->name);
        console_write(" wake=");
        console_write_u32_dec(t->wake_tick);
        console_putc('\n');
    }

    irq_restore(flags);
}

/* ------------------------------------------------------------------------- */
/* Software-yield test                                                        */
/* ------------------------------------------------------------------------- */

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
    console_writeln("Thread test: starting software-yield multitasking test");

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

    thread_reap_zombies();

    console_writeln("Thread test: completed software-yield multitasking test");
    thread_dump_state();
}

/* ------------------------------------------------------------------------- */
/* Preemption test                                                            */
/* ------------------------------------------------------------------------- */

typedef struct preempt_test_arg {
    const char *label;
    volatile uint32_t counter;
} preempt_test_arg_t;

static preempt_test_arg_t preempt_arg_a = {
    .label = "P",
    .counter = 0
};

static preempt_test_arg_t preempt_arg_b = {
    .label = "Q",
    .counter = 0
};

static void preempt_worker(void *arg) {
    preempt_test_arg_t *test = (preempt_test_arg_t *)arg;

    for (uint32_t round = 0; round < 5; ++round) {
        for (volatile uint32_t i = 0; i < 600000u; ++i) {
            test->counter++;
        }

        console_write("Preempt test: worker ");
        console_write(test->label);
        console_write(" round ");
        console_write_u32_dec(round);
        console_putc('\n');
    }

    preempt_test_done_count++;
}

void thread_preemption_test_prepare(void) {
    preempt_test_done_count = 0;
    preempt_arg_a.counter = 0;
    preempt_arg_b.counter = 0;

    console_writeln("Preempt test: creating non-yielding workers");

    thread_create("preempt-p", preempt_worker, &preempt_arg_a);
    thread_create("preempt-q", preempt_worker, &preempt_arg_b);
}

void thread_preemption_test_wait(void) {
    console_writeln("Preempt test: waiting for timer-driven scheduling");

    while (preempt_test_done_count < 2) {
        interrupts_wait();
    }

    console_write("Preempt test: worker P counter ");
    console_write_u32_dec(preempt_arg_a.counter);
    console_putc('\n');

    console_write("Preempt test: worker Q counter ");
    console_write_u32_dec(preempt_arg_b.counter);
    console_putc('\n');

    if (preempt_arg_a.counter == 0 || preempt_arg_b.counter == 0) {
        kernel_panic("preemption test counter did not advance");
    }

    thread_reap_zombies();

    console_writeln("Preempt test: timer-driven preemption sanity check passed");
    thread_dump_state();
}

/* ------------------------------------------------------------------------- */
/* Sleep/blocking test                                                        */
/* ------------------------------------------------------------------------- */

typedef struct sleep_test_arg {
    const char *label;
    uint32_t sleep_ticks;
} sleep_test_arg_t;

static sleep_test_arg_t sleep_arg_a = {
    .label = "S1",
    .sleep_ticks = 4
};

static sleep_test_arg_t sleep_arg_b = {
    .label = "S2",
    .sleep_ticks = 8
};

static void sleep_worker(void *arg) {
    sleep_test_arg_t *test = (sleep_test_arg_t *)arg;

    console_write("Sleep test: worker ");
    console_write(test->label);
    console_write(" sleeping for ");
    console_write_u32_dec(test->sleep_ticks);
    console_writeln(" ticks");

    thread_sleep_ticks(test->sleep_ticks);

    console_write("Sleep test: worker ");
    console_write(test->label);
    console_write(" woke at tick ");
    console_write_u32_dec(thread_ticks());
    console_putc('\n');

    sleep_test_done_count++;
}

void thread_sleep_test_once(void) {
    console_writeln("Sleep test: starting blocking sleep test");

    sleep_test_done_count = 0;

    thread_create("sleep-s1", sleep_worker, &sleep_arg_a);
    thread_create("sleep-s2", sleep_worker, &sleep_arg_b);

    while (sleep_test_done_count < 2) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    console_writeln("Sleep test: blocking sleep sanity check passed");
    thread_dump_state();
}
```

---

# 5. Why the idle thread is necessary

Blocking introduces a new possibility:

```text
all normal threads are sleeping
```

If the scheduler has no ready thread, it still needs something safe to run.

That “something” is the idle thread:

```c
static void idle_thread_entry(void *arg) {
    (void)arg;

    for (;;) {
        interrupts_wait();
    }
}
```

The idle thread does not do work. It simply halts the CPU until the next interrupt.

Without an idle thread, this sequence would break:

```text
bootstrap sleeps
worker A sleeps
worker B sleeps
ready queue empty
timer must still run
```

The idle thread gives the CPU a safe place to wait while timer interrupts wake sleepers.

---

# 6. Update `kernel/kmain.c`

Add the sleep test after the preemption test.

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

    pit_wait_ticks(3);
    console_writeln("Timer: observed 3 ticks");

    console_writeln("Try typing in the QEMU window.");
    console_writeln("Next stop: locks, wait queues, and keyboard input buffering.");

    kernel_idle_forever();
}
```

---

# 7. Update `Makefile`

There are no new `.c` objects for `irq_state.h`, because it is a header-only inline helper.

Your object list from Chapter 10 should still be valid:

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
    build/kernel/thread.o \
    build/kernel/vmem.o \
    build/kernel/lib/mem.o \
    build/drivers/console/serial.o \
    build/drivers/console/vga_text.o \
    build/drivers/input/keyboard.o
```

Update the test greps:

```make
test: iso
	$(GRUB_FILE) --is-x86-multiboot build/kernel.elf
	@mkdir -p build
	@timeout 10s $(QEMU) \
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
	grep -q "Preempt test: timer-driven preemption sanity check passed" build/test.log
	grep -q "Sleep test: blocking sleep sanity check passed" build/test.log
	grep -q "Timer: observed 3 ticks" build/test.log
	@echo "Boot, memory, heap, preemption, and blocking sleep smoke test passed."
```

I increased the timeout to 10 seconds because this chapter runs preemption and sleep tests.

---

# 8. Update `tests/smoke.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail

make clean
make test
make test-exception
make test-page-fault

echo "All Chapter 11 checks passed."
```

Run:

```bash
chmod +x tests/smoke.sh
./tests/smoke.sh
```

---

# 9. Expected output

A successful boot should include lines like:

```text
Threads: blocking scheduler initialized
Thread test: completed software-yield multitasking test
Preempt test: timer-driven preemption sanity check passed
Sleep test: starting blocking sleep test
Sleep test: worker S1 sleeping for 4 ticks
Sleep test: worker S2 sleeping for 8 ticks
Threads: waking sleep-s1 at tick ...
Sleep test: worker S1 woke at tick ...
Threads: waking sleep-s2 at tick ...
Sleep test: worker S2 woke at tick ...
Sleep test: blocking sleep sanity check passed
Timer: observed 3 ticks
```

The exact tick numbers may vary.

The important fact is that sleeping threads stop running and later resume without calling busy loops.

---

# 10. What this chapter achieved

We now have:

```text
preemptive scheduler
idle thread
ready queue
sleep queue
zombie queue
thread_sleep_ticks()
thread_reap_zombies()
interrupt-save/restore critical sections
```

The scheduler model is now much closer to a real kernel:

```text
RUNNING
  ↓ sleep
SLEEPING
  ↓ timer wakeup
READY
  ↓ scheduler picks it
RUNNING
  ↓ exit
ZOMBIE
  ↓ reaper frees stack/object
destroyed
```

That gives us the primitives needed for real blocking operations.

---

# Resources

- [Chapter 11 release](https://github.com/Monotoba/toyix/releases/tag/Chapter_11)
- [OSDev blocking process](https://wiki.osdev.org/Blocking_Process)
- [Brendan's multitasking tutorial](https://wiki.osdev.org/Brendan%27s_Multi-tasking_Tutorial)
- [OSDev context switching](https://wiki.osdev.org/Context_Switching)

# Closure

Chapter 11 turns the scheduler from a cooperative/preemptive proof of concept into a blocking scheduler with an idle thread, a sleep queue, zombie cleanup, and interrupt-safe queue manipulation. That gives us the kernel-side mechanics needed for real locks, wait queues, and eventually blocking device input.

The next chapter can build directly on this by adding wait queues and the first lock-protected handoff between a device producer and a sleeping consumer.

Happy Coding!

[1]: https://wiki.osdev.org/Blocking_Process?utm_source=chatgpt.com "Blocking Process"
[2]: https://wiki.osdev.org/Brendan%27s_Multi-tasking_Tutorial?utm_source=chatgpt.com "Brendan's Multi-tasking Tutorial - OSDev Wiki"
[3]: https://f.osdev.org/viewtopic.php?t=10006&utm_source=chatgpt.com "Interrupting the scheduler... - OSDev.org"
