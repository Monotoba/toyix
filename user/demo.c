#include "toyix_syscall.h"

static toyix_u32 str_len(const char *text) {
    toyix_u32 len = 0;

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

static void write_str(const char *text) {
    toyix_write(FD_STDOUT, text, str_len(text));
}

static void write_uint(toyix_u32 value) {
    char buffer[11];
    toyix_u32 index = 0;

    if (value == 0) {
        write_str("0");
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (index > 0) {
        char ch = buffer[--index];
        toyix_write(FD_STDOUT, &ch, 1);
    }
}

int main(int argc, char **argv) {
    char buffer[32];

    write_str("argc=");
    write_uint((toyix_u32)argc);
    write_str("\n");

    for (int i = 0; i < argc; ++i) {
        write_str("argv[");
        write_uint((toyix_u32)i);
        write_str("]=");
        write_str(argv[i]);
        write_str("\n");
    }

    write_str("user> ");

    toyix_i32 got = toyix_read(FD_STDIN, buffer, sizeof(buffer));

    if (got < 0) {
        write_str("read failed\n");
        return 1;
    }

    write_str("echo: ");

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    write_str("\n");

    toyix_sleep(3);

    return 9;
}
