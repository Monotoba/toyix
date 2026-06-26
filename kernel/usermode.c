// kernel/usermode.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/gdt.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/string.h"
#include "kernel/syscall.h"
#include "kernel/thread.h"
#include "kernel/usermode.h"
#include "kernel/vmem.h"

#define USER_TEST_CODE_VA   0x40000000u
#define USER_TEST_STACK_VA  0x40001000u
#define USER_TEST_STACK_TOP 0x40002000u

extern void x86_enter_user_mode(uint32_t user_eip, uint32_t user_esp);

static volatile uint32_t usermode_test_done;
static volatile uint32_t usermode_exit_code;
static int user_pages_mapped;

static const uint8_t user_test_program[] = {
    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, 'U',      0x00, 0x00, 0x00,
    0xCD, 0x80,

    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, '3',      0x00, 0x00, 0x00,
    0xCD, 0x80,

    0xB8, SYS_PUTC, 0x00, 0x00, 0x00,
    0xBB, '\n',     0x00, 0x00, 0x00,
    0xCD, 0x80,

    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0x31, 0xDB,
    0xCD, 0x80,

    0xEB, 0xFE
};

static void map_user_test_pages(void) {
    if (user_pages_mapped) {
        return;
    }

    uintptr_t code_phys = pmm_alloc_page();
    uintptr_t stack_phys = pmm_alloc_page();

    if (code_phys == PMM_INVALID_PAGE ||
        stack_phys == PMM_INVALID_PAGE) {
        kernel_panic("user mode test could not allocate physical pages");
    }

    int rc = vmem_map_page(
        USER_TEST_CODE_VA,
        code_phys,
        VMEM_FLAG_WRITABLE | VMEM_FLAG_USER
    );

    if (rc != VMEM_OK) {
        kernel_panic("user mode test could not map code page");
    }

    rc = vmem_map_page(
        USER_TEST_STACK_VA,
        stack_phys,
        VMEM_FLAG_WRITABLE | VMEM_FLAG_USER
    );

    if (rc != VMEM_OK) {
        kernel_panic("user mode test could not map stack page");
    }

    memset((void *)USER_TEST_CODE_VA, 0x90, PMM_PAGE_SIZE);
    memcpy((void *)USER_TEST_CODE_VA, user_test_program, sizeof(user_test_program));

    memset((void *)USER_TEST_STACK_VA, 0, PMM_PAGE_SIZE);

    user_pages_mapped = 1;
}

static uint32_t current_thread_kernel_stack_top(void) {
    thread_t *self = thread_current();

    if (self == 0 || self->stack_base == 0) {
        kernel_panic("user mode entry requires current thread stack");
    }

    return (uint32_t)((uintptr_t)self->stack_base + self->stack_size);
}

static void user_test_thread(void *arg) {
    (void)arg;

    console_writeln("User mode test: entering ring 3");

    tss_set_kernel_stack(current_thread_kernel_stack_top());
    x86_enter_user_mode(USER_TEST_CODE_VA, USER_TEST_STACK_TOP);

    kernel_panic("x86_enter_user_mode returned unexpectedly");
}

void usermode_notify_exit(uint32_t exit_code) {
    usermode_exit_code = exit_code;
    usermode_test_done = 1;
}

void usermode_test_once(void) {
    console_writeln("User mode test: preparing ring 3 program");

    usermode_test_done = 0;
    usermode_exit_code = 0xFFFFFFFFu;

    map_user_test_pages();

    thread_create("user-test", user_test_thread, 0);

    while (!usermode_test_done) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    if (usermode_exit_code != 0) {
        kernel_panic("user mode test returned nonzero exit code");
    }

    console_writeln("User mode test: ring 3 syscall/exit sanity check passed");
}
