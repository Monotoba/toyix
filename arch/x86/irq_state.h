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
