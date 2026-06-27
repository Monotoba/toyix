// user/counter.c
#include "toyix.h"

int main(int argc, char **argv) {
    toyix_write_str("counter: argc=");
    toyix_write_int(argc);
    toyix_putchar('\n');

    for (int i = 0; i < argc; ++i) {
        toyix_write_str("counter: argv[");
        toyix_write_int(i);
        toyix_write_str("]=");
        toyix_puts(argv[i]);
    }

    for (toyix_u32 i = 1; i <= 3; ++i) {
        toyix_write_str("counter: tick ");
        toyix_write_uint(i);
        toyix_putchar('\n');

        toyix_sleep(2);
    }

    return 4;
}
