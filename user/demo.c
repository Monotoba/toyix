#include "toyix.h"

int main(int argc, char **argv) {
    char buffer[32];

    toyix_write_str("argc=");
    toyix_write_int(argc);
    toyix_putchar('\n');

    for (int i = 0; i < argc; ++i) {
        toyix_write_str("argv[");
        toyix_write_int(i);
        toyix_write_str("]=");
        toyix_puts(argv[i]);
    }

    toyix_write_str("user> ");

    toyix_i32 got = toyix_read(FD_STDIN, buffer, sizeof(buffer));

    if (got < 0) {
        toyix_puts("read failed");
        return 1;
    }

    toyix_write_str("echo: ");

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_putchar('\n');

    toyix_sleep(3);

    return 9;
}
