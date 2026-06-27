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

void toyix_write_hex(toyix_u32 value) {
    char buffer[8];
    toyix_u32 index = 0;

    if (value == 0) {
        toyix_putchar('0');
        return;
    }

    while (value != 0 && index < sizeof(buffer)) {
        toyix_u32 nibble = value & 0xFu;
        if (nibble < 10u) {
            buffer[index++] = (char)('0' + nibble);
        } else {
            buffer[index++] = (char)('a' + (nibble - 10u));
        }
        value >>= 4u;
    }

    while (index > 0) {
        toyix_putchar(buffer[--index]);
    }
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

int toyix_strcmp(const char *a, const char *b) {
    if (a == 0 && b == 0) {
        return 0;
    }

    if (a == 0) {
        return -1;
    }

    if (b == 0) {
        return 1;
    }

    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return (int)(unsigned char)(*a) -
                   (int)(unsigned char)(*b);
        }

        ++a;
        ++b;
    }

    return (int)(unsigned char)(*a) -
           (int)(unsigned char)(*b);
}

int toyix_isspace(char ch) {
    return ch == ' ' ||
           ch == '\t' ||
           ch == '\r' ||
           ch == '\n';
}

int toyix_atoi(const char *text, toyix_i32 *out_value) {
    int negative = 0;
    toyix_i32 value = 0;

    if (text == 0 || out_value == 0) {
        return 0;
    }

    while (toyix_isspace(*text)) {
        ++text;
    }

    if (*text == '-') {
        negative = 1;
        ++text;
    } else if (*text == '+') {
        ++text;
    }

    if (*text < '0' || *text > '9') {
        return 0;
    }

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (toyix_i32)(*text - '0');
        ++text;
    }

    while (toyix_isspace(*text)) {
        ++text;
    }

    if (*text != '\0') {
        return 0;
    }

    *out_value = negative ? -value : value;
    return 1;
}

toyix_i32 toyix_readline(char *buffer, toyix_u32 size) {
    toyix_i32 got;

    if (buffer == 0 || size == 0) {
        return -1;
    }

    if (size == 1u) {
        buffer[0] = '\0';
        return 0;
    }

    got = toyix_read(FD_STDIN, buffer, size - 1u);
    if (got < 0) {
        buffer[0] = '\0';
        return got;
    }

    if ((toyix_u32)got >= size) {
        got = (toyix_i32)(size - 1u);
    }

    buffer[got] = '\0';
    return got;
}

void toyix_vprintf(const char *format, toyix_va_list ap) {
    if (format == 0) {
        return;
    }

    while (*format != '\0') {
        if (*format != '%') {
            toyix_putchar(*format);
            ++format;
            continue;
        }

        ++format;

        if (*format == '\0') {
            toyix_putchar('%');
            return;
        }

        switch (*format) {
            case '%':
                toyix_putchar('%');
                break;
            case 'c':
                toyix_putchar((char)toyix_va_arg(ap, int));
                break;
            case 'd':
                toyix_write_int((toyix_i32)toyix_va_arg(ap, int));
                break;
            case 'u':
                toyix_write_uint((toyix_u32)toyix_va_arg(ap, unsigned int));
                break;
            case 'x':
                toyix_write_hex((toyix_u32)toyix_va_arg(ap, unsigned int));
                break;
            case 's': {
                const char *text = toyix_va_arg(ap, const char *);
                toyix_write_str(text != 0 ? text : "(null)");
                break;
            }
            default:
                toyix_putchar('%');
                toyix_putchar(*format);
                break;
        }

        ++format;
    }
}

void toyix_printf(const char *format, ...) {
    toyix_va_list ap;

    toyix_va_start(ap, format);
    toyix_vprintf(format, ap);
    toyix_va_end(ap);
}
