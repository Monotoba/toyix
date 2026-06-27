// include/kernel/process.h
#ifndef TOYIX_KERNEL_PROCESS_H
#define TOYIX_KERNEL_PROCESS_H

#include <stdint.h>
#include "kernel/address_space.h"

struct thread;

typedef enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_ZOMBIE,
    PROCESS_DESTROYED
} process_state_t;

typedef struct process {
    uint32_t magic;
    uint32_t pid;
    uint32_t parent_pid;

    const char *name;
    process_state_t state;

    address_space_t *address_space;

    struct thread *main_thread;

    uint32_t exit_code;
    int exited;
    int kill_requested;

    uintptr_t user_code_base;
    uintptr_t user_entry;

    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
    uintptr_t user_initial_esp;

    struct process *next;
    struct process *prev;
} process_t;

void process_init_system(void);

process_t *process_create_empty(const char *name);

int process_map_user_region(
    process_t *process,
    uintptr_t virtual_addr,
    uint32_t size_bytes
);

int process_copy_to_user_init(
    process_t *process,
    uintptr_t user_dest,
    const void *kernel_src,
    uint32_t size
);

int process_zero_user_init(
    process_t *process,
    uintptr_t user_dest,
    uint32_t size
);

void process_set_user_entry(process_t *process, uintptr_t entry);
void process_set_user_stack(
    process_t *process,
    uintptr_t stack_base,
    uintptr_t stack_top
);

int process_setup_arguments(
    process_t *process,
    int argc,
    const char **argv
);

void process_set_parent(process_t *process, uint32_t parent_pid);
uint32_t process_parent_pid(process_t *process);
int process_is_child_of(process_t *process, uint32_t parent_pid);

#define PROCESS_KILLED_EXIT_CODE 128u

void process_request_kill(process_t *process);
int process_kill_requested(process_t *process);
int process_request_kill_child(uint32_t parent_pid, uint32_t child_pid);

void process_start_user(process_t *process);

process_t *process_current(void);

void process_exit_current(uint32_t exit_code);

process_t *process_find(uint32_t pid);
void process_list(void);
const char *process_state_name(process_state_t state);

uint32_t process_wait(process_t *process);
void process_destroy(process_t *process);

uint32_t process_last_exit_code(void);
int process_last_exit_seen(void);

#endif
