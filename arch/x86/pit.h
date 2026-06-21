// arch/x86/pit.h
#ifndef TOYIX_ARCH_X86_PIT_H
#define TOYIX_ARCH_X86_PIT_H

#include <stdint.h>


void pit_init(uint32_t frequency_hz);
uint32_t pit_get_ticks(void);
void pit_wait_ticks(uint32_t ticks);


#endif
