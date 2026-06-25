#ifndef TOYIX_KERNEL_MONITOR_H
#define TOYIX_KERNEL_MONITOR_H

void monitor_init(void);
void monitor_start(void);

int monitor_execute_command(const char *line);

void monitor_test_once(void);

#endif
