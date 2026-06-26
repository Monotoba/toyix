#include <stdint.h>
#include "arch/x86/interrupts.h"
#include "arch/x86/io.h"
#include "arch/x86/irq_state.h"
#include "arch/x86/pic.h"
#include "drivers/input/keyboard.h"
#include "kernel/console.h"
#include "kernel/panic.h"
#include "kernel/thread.h"
#include "kernel/wait_queue.h"

#define KEYBOARD_DATA_PORT 0x60u
#define KEYBOARD_RING_SIZE 128u

#define SC_LSHIFT   0x2Au
#define SC_RSHIFT   0x36u
#define SC_CAPSLOCK 0x3Au

static wait_queue_t keyboard_wait_queue;

static char keyboard_ring[KEYBOARD_RING_SIZE];
static uint32_t keyboard_head;
static uint32_t keyboard_tail;
static uint32_t keyboard_count;

static int keyboard_shift_down;
static int keyboard_caps_lock;

static volatile uint32_t keyboard_test_done;
static char keyboard_test_result[4];

static const char scancode_set1_ascii[128] = {
	[0x01] = 0,
	[0x02] = '1',
	[0x03] = '2',
	[0x04] = '3',
	[0x05] = '4',
	[0x06] = '5',
	[0x07] = '6',
	[0x08] = '7',
	[0x09] = '8',
	[0x0A] = '9',
	[0x0B] = '0',
	[0x0C] = '-',
	[0x0D] = '=',
	[0x0E] = '\b',
	[0x0F] = '\t',

	[0x10] = 'q',
	[0x11] = 'w',
	[0x12] = 'e',
	[0x13] = 'r',
	[0x14] = 't',
	[0x15] = 'y',
	[0x16] = 'u',
	[0x17] = 'i',
	[0x18] = 'o',
	[0x19] = 'p',
	[0x1A] = '[',
	[0x1B] = ']',
	[0x1C] = '\n',

	[0x1E] = 'a',
	[0x1F] = 's',
	[0x20] = 'd',
	[0x21] = 'f',
	[0x22] = 'g',
	[0x23] = 'h',
	[0x24] = 'j',
	[0x25] = 'k',
	[0x26] = 'l',
	[0x27] = ';',
	[0x28] = '\'',
	[0x29] = '`',

	[0x2B] = '\\',
	[0x2C] = 'z',
	[0x2D] = 'x',
	[0x2E] = 'c',
	[0x2F] = 'v',
	[0x30] = 'b',
	[0x31] = 'n',
	[0x32] = 'm',
	[0x33] = ',',
	[0x34] = '.',
	[0x35] = '/',
	[0x39] = ' '
};

static const char scancode_set1_shifted_ascii[128] = {
	[0x02] = '!',
	[0x03] = '@',
	[0x04] = '#',
	[0x05] = '$',
	[0x06] = '%',
	[0x07] = '^',
	[0x08] = '&',
	[0x09] = '*',
	[0x0A] = '(',
	[0x0B] = ')',
	[0x0C] = '_',
	[0x0D] = '+',

	[0x1A] = '{',
	[0x1B] = '}',

	[0x27] = ':',
	[0x28] = '"',
	[0x29] = '~',

	[0x2B] = '|',
	[0x33] = '<',
	[0x34] = '>',
	[0x35] = '?'
};

static int keyboard_has_data(void *context) {
	(void)context;
	return keyboard_count > 0;
}

static int ascii_is_lower(char ch) {
	return ch >= 'a' && ch <= 'z';
}

static int ascii_is_upper(char ch) {
	return ch >= 'A' && ch <= 'Z';
}

static char ascii_to_upper(char ch) {
	if (ascii_is_lower(ch)) {
		return (char)(ch - 'a' + 'A');
	}

	return ch;
}

static char keyboard_translate_scancode(uint8_t scancode) {
	char base = scancode_set1_ascii[scancode];

	if (base == 0) {
		return 0;
	}

	if (ascii_is_lower(base) || ascii_is_upper(base)) {
		int upper = keyboard_shift_down ^ keyboard_caps_lock;
		return upper ? ascii_to_upper(base) : base;
	}

	if (keyboard_shift_down && scancode_set1_shifted_ascii[scancode] != 0) {
		return scancode_set1_shifted_ascii[scancode];
	}

	return base;
}

static int keyboard_ring_push(char ch) {
	if (keyboard_count >= KEYBOARD_RING_SIZE) {
		return 0;
	}

	keyboard_ring[keyboard_tail] = ch;
	keyboard_tail = (keyboard_tail + 1u) % KEYBOARD_RING_SIZE;
	keyboard_count++;

	return 1;
}

static int keyboard_ring_pop(char *out) {
	if (keyboard_count == 0 || out == 0) {
		return 0;
	}

	*out = keyboard_ring[keyboard_head];
	keyboard_head = (keyboard_head + 1u) % KEYBOARD_RING_SIZE;
	keyboard_count--;

	return 1;
}

static void keyboard_deliver_char(char ch) {
	irq_flags_t flags = irq_save();
	int pushed = keyboard_ring_push(ch);
	irq_restore(flags);

	if (pushed) {
		wait_queue_wake_one(&keyboard_wait_queue);
	}
}

static void keyboard_handle_modifier(uint8_t scancode) {
	int released = (scancode & 0x80u) != 0;
	uint8_t code = scancode & 0x7Fu;

	if (code == SC_LSHIFT || code == SC_RSHIFT) {
		keyboard_shift_down = released ? 0 : 1;
		return;
	}

	if (!released && code == SC_CAPSLOCK) {
		keyboard_caps_lock = !keyboard_caps_lock;
		return;
	}
}

static int keyboard_is_modifier(uint8_t scancode) {
	uint8_t code = scancode & 0x7Fu;

	return code == SC_LSHIFT ||
	       code == SC_RSHIFT ||
	       code == SC_CAPSLOCK;
}

static void keyboard_irq_handler(interrupt_frame_t *frame) {
	(void)frame;

	uint8_t scancode = inb(KEYBOARD_DATA_PORT);

	if (keyboard_is_modifier(scancode)) {
		keyboard_handle_modifier(scancode);
		return;
	}

	/*
	 * In Set 1 style scancodes, high bit marks key release.
	 * This driver ignores non-modifier releases.
	 */
	if ((scancode & 0x80u) != 0) {
		return;
	}

	if (scancode < 128) {
		char ch = keyboard_translate_scancode(scancode);

		if (ch != 0) {
			keyboard_deliver_char(ch);
		}
	}
}

int keyboard_init(void) {
	keyboard_head = 0;
	keyboard_tail = 0;
	keyboard_count = 0;
	keyboard_shift_down = 0;
	keyboard_caps_lock = 0;
	keyboard_test_done = 0;
	keyboard_test_result[0] = 0;
	keyboard_test_result[1] = 0;
	keyboard_test_result[2] = 0;
	keyboard_test_result[3] = 0;

	wait_queue_init(&keyboard_wait_queue, "keyboard");

	interrupt_register_handler(33, keyboard_irq_handler);
	pic_clear_mask(1);

	console_writeln("Keyboard: IRQ1 handler, modifiers, and input buffer installed");
	return 0;
}

int keyboard_try_getchar(char *out) {
	if (out == 0) {
		return 0;
	}

	irq_flags_t flags = irq_save();
	int ok = keyboard_ring_pop(out);
	irq_restore(flags);

	return ok;
}

char keyboard_getchar_blocking(void) {
	char ch;

	for (;;) {
		wait_queue_wait(&keyboard_wait_queue, keyboard_has_data, 0);

		irq_flags_t flags = irq_save();
		int ok = keyboard_ring_pop(&ch);
		irq_restore(flags);

		if (ok) {
			return ch;
		}
	}
}

void keyboard_debug_inject_char(char ch) {
	keyboard_deliver_char(ch);
}

static void keyboard_reader_test_thread(void *arg) {
	(void)arg;

	keyboard_test_result[0] = keyboard_getchar_blocking();
	keyboard_test_result[1] = keyboard_getchar_blocking();
	keyboard_test_result[2] = keyboard_getchar_blocking();
	keyboard_test_result[3] = '\0';

	console_write("Keyboard test: reader received ");
	console_writeln(keyboard_test_result);

	keyboard_test_done = 1;
}

void keyboard_buffer_test_once(void) {
	console_writeln("Keyboard test: starting blocking input-buffer test");

	keyboard_test_done = 0;
	keyboard_test_result[0] = 0;
	keyboard_test_result[1] = 0;
	keyboard_test_result[2] = 0;
	keyboard_test_result[3] = 0;

	thread_create("kbd-reader", keyboard_reader_test_thread, 0);

	/*
	 * Let the reader run and block on the empty keyboard queue.
	 */
	thread_sleep_ticks(2);

	keyboard_debug_inject_char('o');
	thread_sleep_ticks(1);

	keyboard_debug_inject_char('s');
	thread_sleep_ticks(1);

	keyboard_debug_inject_char('!');
	thread_sleep_ticks(1);

	while (!keyboard_test_done) {
		thread_sleep_ticks(1);
		thread_reap_zombies();
	}

	thread_reap_zombies();

	if (keyboard_test_result[0] != 'o' ||
	    keyboard_test_result[1] != 's' ||
	    keyboard_test_result[2] != '!') {
		kernel_panic("keyboard blocking buffer test failed");
	}

	console_writeln("Keyboard test: blocking input-buffer sanity check passed");
}
