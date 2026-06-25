// drivers/input/keyboard.h
#ifndef TOYIX_DRIVERS_INPUT_KEYBOARD_H
#define TOYIX_DRIVERS_INPUT_KEYBOARD_H

int keyboard_init(void);
int keyboard_try_getchar(char *out);
char keyboard_getchar_blocking(void);
void keyboard_debug_inject_char(char ch);
void keyboard_buffer_test_once(void);

#endif
