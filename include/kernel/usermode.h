// include/kernel/usermode.h
#ifndef TOYIX_KERNEL_USERMODE_H
#define TOYIX_KERNEL_USERMODE_H

#include <stdint.h>

void usermode_test_once(void);
void usermode_notify_exit(uint32_t exit_code);

#endif
