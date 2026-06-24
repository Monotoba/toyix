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

void thread_on_timer_tick(interrupt_frame_t *frame);
int thread_should_reschedule(void);
uintptr_t thread_schedule_from_interrupt(interrupt_frame_t *frame);
uintptr_t schedule_interrupt_handler(interrupt_frame_t *frame);

void thread_preemption_init(uint32_t ticks_per_slice);
void thread_preemption_test_prepare(void);
void thread_preemption_test_wait(void);

uint32_t thread_ready_count(void);
void thread_dump_state(void);
void thread_test_once(void);

#endif
