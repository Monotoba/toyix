#include "toyix.h"

static const char *file_type_name(toyix_u32 type) {
    switch (type) {
        case TOYIX_FILE_REGULAR:
            return "file";
        case TOYIX_FILE_DIRECTORY:
            return "directory";
        default:
            return "unknown";
    }
}

static void print_chunk(const char *label, const char *buffer, toyix_i32 got) {
    toyix_printf("%s", label);

    if (got > 0) {
        toyix_write(FD_STDOUT, buffer, (toyix_u32)got);
    }

    toyix_putchar('\n');
}

int main(void) {
    char buffer[16];
    toyix_i32 fd;
    toyix_i32 got;
    toyix_stat_t stat;

    toyix_puts("fstest: starting filesystem smoke test");

    fd = toyix_open("/README", 0);
    if (fd < 0) {
        toyix_puts("fstest: could not open /README");
        return 1;
    }

    got = toyix_read((toyix_u32)fd, buffer, 8u);
    if (got < 0) {
        toyix_puts("fstest: first read failed");
        toyix_close((toyix_u32)fd);
        return 2;
    }

    print_chunk("fstest: README first read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, 0, TOYIX_SEEK_SET) < 0) {
        toyix_puts("fstest: rewind failed");
        toyix_close((toyix_u32)fd);
        return 3;
    }

    got = toyix_read((toyix_u32)fd, buffer, 8u);
    if (got < 0) {
        toyix_puts("fstest: rewind read failed");
        toyix_close((toyix_u32)fd);
        return 4;
    }

    print_chunk("fstest: README rewind read: ", buffer, got);

    if (toyix_seek((toyix_u32)fd, 6, TOYIX_SEEK_SET) < 0) {
        toyix_puts("fstest: skip seek failed");
        toyix_close((toyix_u32)fd);
        return 5;
    }

    got = toyix_read((toyix_u32)fd, buffer, 5u);
    if (got < 0) {
        toyix_puts("fstest: skip read failed");
        toyix_close((toyix_u32)fd);
        return 6;
    }

    print_chunk("fstest: README skip read: ", buffer, got);

    if (toyix_close((toyix_u32)fd) != 0) {
        toyix_puts("fstest: close failed");
        return 7;
    }

    if (toyix_stat("/README", &stat) != 0) {
        toyix_puts("fstest: stat failed for /README");
        return 8;
    }

    toyix_printf(
        "fstest: README type=%s size=%u\n",
        file_type_name(stat.type),
        stat.size
    );

    if (toyix_stat("/programs", &stat) != 0) {
        toyix_puts("fstest: stat failed for /programs");
        return 9;
    }

    toyix_printf(
        "fstest: /programs type=%s size=%u\n",
        file_type_name(stat.type),
        stat.size
    );

    if (toyix_stat("/missing", &stat) == 0) {
        toyix_puts("fstest: missing path unexpectedly succeeded");
        return 10;
    }

    toyix_puts("fstest: missing path check passed");
    toyix_puts("fstest: completed filesystem smoke test");
    return 0;
}
