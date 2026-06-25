// kernel/console.c
#include <stddef.h>
#include <stdint.h>
#include "arch/x86/irq_state.h"
#include "kernel/console.h"
#include "kernel/sync.h"
#include "kernel/thread.h"

#define MAX_CONSOLE_DRIVERS 4

static const console_driver_t *drivers[MAX_CONSOLE_DRIVERS];
static size_t driver_count = 0;
static mutex_t console_mutex;
static int console_locking_ready;

void console_register(const console_driver_t * driver) {
    if (driver == NULL) {
        return;
    }
    
    if (driver_count >= MAX_CONSOLE_DRIVERS) {
        return;
    }
    
    drivers[driver_count++] = driver;
}


void console_init_all(void) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->init != NULL) {
            drivers[i]->init();
        }
    }
}


void console_locking_init(void) {
    mutex_init(&console_mutex, "console");
    console_locking_ready = 1;
    console_writeln("Console: output mutex enabled");
}


void console_lock(void) {
    irq_flags_t flags = irq_save();
    int can_lock = console_locking_ready && irq_flags_interrupts_enabled(flags);
    irq_restore(flags);

    if (can_lock) {
        mutex_lock(&console_mutex);
    }
}


void console_unlock(void) {
    irq_flags_t flags = irq_save();
    int can_unlock = console_locking_ready && irq_flags_interrupts_enabled(flags);
    irq_restore(flags);

    if (can_unlock) {
        mutex_unlock(&console_mutex);
    }
}


void console_raw_putc(char c) {
    for (size_t i = 0; i < driver_count; ++i) {
        if (drivers[i]->putc != NULL) {
            drivers[i]->putc(c);
        }
    }

}


void console_raw_write(const char *text) {
    if (text == NULL) {
        return;
    }
    
    while (*text != '\0') {
        console_raw_putc(*text++);
    }
}


void console_raw_writeln(const char *text) {
    console_raw_write(text);
    console_raw_putc('\n');
}



void console_raw_write_hex32(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    
    console_raw_write("0x");
    
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        console_raw_putc(digits[nibble]);
    }
}



void console_raw_write_u32_dec(uint32_t value) {
	char buffer[11];
	size_t index = 0;
	
	if (value == 0) {
		console_raw_putc('0');
		return;
	}
	
	while (value > 0 && index < sizeof(buffer)) {
		buffer[index++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	
	while (index > 0) {
		console_raw_putc(buffer[--index]);
	}
}


void console_putc(char c) {
    console_lock();
    console_raw_putc(c);
    console_unlock();
}


void console_write(const char *text) {
    console_lock();
    console_raw_write(text);
    console_unlock();
}


void console_writeln(const char *text) {
    console_lock();
    console_raw_writeln(text);
    console_unlock();
}


void console_write_hex32(uint32_t value) {
    console_lock();
    console_raw_write_hex32(value);
    console_unlock();
}


void console_write_u32_dec(uint32_t value) {
    console_lock();
    console_raw_write_u32_dec(value);
    console_unlock();
}


typedef struct console_test_arg {
    const char *label;
    volatile uint32_t *done_counter;
} console_test_arg_t;


static void console_test_worker(void *arg) {
    console_test_arg_t *test = (console_test_arg_t *)arg;

    for (uint32_t i = 0; i < 5; ++i) {
        console_lock();

        console_raw_write("Console lock test: ");
        console_raw_write(test->label);
        console_raw_write(" complete line ");
        console_raw_write_u32_dec(i);
        console_raw_putc('\n');

        console_unlock();

        thread_sleep_ticks(1);
    }

    (*test->done_counter)++;
}


void console_lock_test_once(void) {
    console_writeln("Console lock test: starting");

    static volatile uint32_t done;
    static console_test_arg_t arg_a;
    static console_test_arg_t arg_b;

    done = 0;

    arg_a.label = "C1";
    arg_a.done_counter = &done;

    arg_b.label = "C2";
    arg_b.done_counter = &done;

    thread_create("console-c1", console_test_worker, &arg_a);
    thread_create("console-c2", console_test_worker, &arg_b);

    while (done < 2) {
        thread_sleep_ticks(1);
        thread_reap_zombies();
    }

    thread_reap_zombies();

    console_writeln("Console lock test: non-interleaved line output sanity check passed");
}
