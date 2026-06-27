#include "toyix.h"

toyix_size_t toyix_strlen(const char *text) {
    toyix_size_t len = 0;

    if (text == 0) {
        return 0;
    }

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

void toyix_putchar(char ch) {
    toyix_write(FD_STDOUT, &ch, 1);
}

void toyix_write_str(const char *text) {
    if (text == 0) {
        return;
    }

    toyix_write(FD_STDOUT, text, (toyix_u32)toyix_strlen(text));
}

void toyix_puts(const char *text) {
    toyix_write_str(text);
    toyix_putchar('\n');
}

void toyix_write_uint(toyix_u32 value) {
    char buffer[11];
    toyix_u32 index = 0;

    if (value == 0) {
        toyix_putchar('0');
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        toyix_putchar(buffer[--index]);
    }
}

void toyix_write_int(toyix_i32 value) {
    if (value < 0) {
        toyix_putchar('-');

        /*
         * Avoid relying on undefined behavior for INT_MIN by converting
         * through unsigned arithmetic.
         */
        toyix_u32 magnitude = (toyix_u32)(-(value + 1)) + 1u;
        toyix_write_uint(magnitude);
        return;
    }

    toyix_write_uint((toyix_u32)value);
}

int toyix_streq(const char *a, const char *b) {
    if (a == 0 || b == 0) {
        return a == b;
    }

    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == *b;
}
