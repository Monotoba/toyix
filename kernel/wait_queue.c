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

static uint32_t wait_queue_count_locked(wait_queue_t *queue) {
	uint32_t count = 0;

	for (thread_t *thread = queue->head; thread != 0; thread = thread->next) {
		count++;
	}

	return count;
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
	uint32_t count = wait_queue_count_locked(queue);
	irq_restore(flags);

	return count;
}
