// arch/x86/pit.c
#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/pic.h"
#include "arch/x86/pit.h"
#include "kernel/console.h"

#define PIT_CHANNEL0_PORT 0x40u
#define PIT_COMMAND_PORT  0x43u
#define PIT_INPUT_HZ  	  1193182u

static volatile uint32_t pit_ticks;


static void pit_irq_handler(interrupt_frame_t *frame) {
	(void)frame;
	pit_ticks++;
}


void pit_init(uint32_t frequency_hz) {
	if (frequency_hz == 0) {
		frequency_hz = 100;
	}
	
	uint32_t divisor = PIT_INPUT_HZ / frequency_hz;
	
	if (divisor == 0) {
		divisor = 1;
	}
	
	if (divisor > 0xFFFFu) {
		divisor = 0xFFFFu;
	}
	
	pit_ticks = 0;
	
	interrupt_register_handler(32, pit_irq_handler);
	
	/*
	 * command byte 0x36:
	 *
	 * Channel 0
	 * Access mode: low byte then high byte
	 * Operating mode 3: square wave generator
	 * binary counter
	 */
	outb(PIT_COMMAND_PORT, 0x36);
	outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFFu));
	outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));
	
	pic_clear_mask(0);
	
	console_write("PIT: timer running at ");
	console_write_hex32(frequency_hz);
	console_writeln(" Hz");
}


uint32_t pit_get_ticks(void) {
	return pit_ticks;
}


void pit_wait_ticks(uint32_t ticks) {
	uint32_t start = pit_get_ticks();
	
	while ((uint32_t)(pit_get_ticks() - start) < ticks) {
		interrupts_wait();
	}
}

