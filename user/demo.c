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

int main(void) {
    char buffer[32];

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
