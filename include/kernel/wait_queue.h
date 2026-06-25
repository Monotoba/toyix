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
