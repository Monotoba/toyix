// kernel/vfs.c
#include <stddef.h>
#include <stdint.h>
#include "kernel/console.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/string.h"
#include "kernel/vfs.h"

typedef struct ramfs_node {
    const char *path;
    uint32_t type;
    const uint8_t *data;
    uint32_t size;
} ramfs_node_t;

struct vfs_file {
    const ramfs_node_t *node;
    uint32_t offset;
};

static const uint8_t readme_text[] =
    "Toyix RAMFS\n"
    "This file lives inside the kernel image.\n"
    "The first filesystem is read-only and memory-backed.\n";

static const uint8_t programs_text[] =
    "demo\n"
    "counter\n"
    "shell\n";

static const ramfs_node_t ramfs_nodes[] = {
    {
        .path = "/README",
        .type = VFS_NODE_REGULAR,
        .data = readme_text,
        .size = sizeof(readme_text) - 1u
    },
    {
        .path = "/programs",
        .type = VFS_NODE_REGULAR,
        .data = programs_text,
        .size = sizeof(programs_text) - 1u
    }
};

static const uint32_t ramfs_node_count =
    sizeof(ramfs_nodes) / sizeof(ramfs_nodes[0]);

void vfs_init(void) {
    console_write("VFS: initialized RAMFS with ");
    console_write_u32_dec(ramfs_node_count);
    console_writeln(" file(s)");
}

static const ramfs_node_t *ramfs_find(const char *path) {
    if (path == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < ramfs_node_count; ++i) {
        if (kstrcmp(path, ramfs_nodes[i].path) == 0) {
            return &ramfs_nodes[i];
        }
    }

    return 0;
}

int vfs_open(const char *path, vfs_file_t **out_file) {
    if (path == 0 || out_file == 0) {
        return VFS_ERR_INVALID;
    }

    const ramfs_node_t *node = ramfs_find(path);

    if (node == 0) {
        return VFS_ERR_NOT_FOUND;
    }

    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));

    if (file == 0) {
        return VFS_ERR_NO_MEMORY;
    }

    file->node = node;
    file->offset = 0;

    *out_file = file;
    return VFS_OK;
}

int vfs_read(
    vfs_file_t *file,
    void *buffer,
    uint32_t length,
    uint32_t *out_read
) {
    if (file == 0 || buffer == 0 || out_read == 0) {
        return VFS_ERR_INVALID;
    }

    *out_read = 0;

    if (length == 0) {
        return VFS_OK;
    }

    if (file->offset >= file->node->size) {
        return VFS_OK;
    }

    uint32_t remaining = file->node->size - file->offset;
    uint32_t to_copy = length;

    if (to_copy > remaining) {
        to_copy = remaining;
    }

    memcpy(buffer, file->node->data + file->offset, to_copy);

    file->offset += to_copy;
    *out_read = to_copy;

    return VFS_OK;
}

int vfs_seek(
    vfs_file_t *file,
    int32_t offset,
    uint32_t whence,
    uint32_t *out_position
) {
    if (file == 0 || out_position == 0) {
        return VFS_ERR_INVALID;
    }

    int64_t base = 0;

    switch (whence) {
        case TOYIX_SEEK_SET:
            base = 0;
            break;

        case TOYIX_SEEK_CUR:
            base = (int64_t)file->offset;
            break;

        case TOYIX_SEEK_END:
            base = (int64_t)file->node->size;
            break;

        default:
            return VFS_ERR_INVALID;
    }

    int64_t next = base + (int64_t)offset;

    if (next < 0 || next > 0xFFFFFFFFLL) {
        return VFS_ERR_INVALID;
    }

    file->offset = (uint32_t)next;
    *out_position = file->offset;

    return VFS_OK;
}

uint32_t vfs_tell(vfs_file_t *file) {
    if (file == 0) {
        return 0;
    }

    return file->offset;
}

uint32_t vfs_size(vfs_file_t *file) {
    if (file == 0 || file->node == 0) {
        return 0;
    }

    return file->node->size;
}

int vfs_stat(const char *path, vfs_stat_t *out_stat) {
    if (path == 0 || out_stat == 0) {
        return VFS_ERR_INVALID;
    }

    const ramfs_node_t *node = ramfs_find(path);

    if (node == 0) {
        return VFS_ERR_NOT_FOUND;
    }

    out_stat->type = node->type;
    out_stat->size = node->size;

    return VFS_OK;
}

void vfs_close(vfs_file_t *file) {
    if (file == 0) {
        return;
    }

    kfree(file);
}

void vfs_test_once(void) {
    console_writeln("VFS test: starting RAMFS open/read/seek/stat/close test");

    vfs_stat_t stat;

    if (vfs_stat("/README", &stat) != VFS_OK) {
        kernel_panic("VFS test could not stat /README");
    }

    if (stat.type != VFS_NODE_REGULAR || stat.size == 0u) {
        kernel_panic("VFS test received invalid /README stat");
    }

    console_write("VFS test: /README size=");
    console_write_u32_dec(stat.size);
    console_writeln(" type=file");

    vfs_file_t *file = 0;

    if (vfs_open("/README", &file) != VFS_OK || file == 0) {
        kernel_panic("VFS test could not open /README");
    }

    char buffer[16];
    uint32_t got = 0;

    if (vfs_read(file, buffer, 8u, &got) != VFS_OK || got != 8u) {
        kernel_panic("VFS test could not read first bytes");
    }

    buffer[got] = '\0';

    console_write("VFS test: first bytes: ");
    console_writeln(buffer);

    uint32_t pos = 0;

    if (vfs_seek(file, 0, TOYIX_SEEK_SET, &pos) != VFS_OK || pos != 0u) {
        kernel_panic("VFS test could not rewind");
    }

    if (vfs_read(file, buffer, 8u, &got) != VFS_OK || got != 8u) {
        kernel_panic("VFS test could not reread first bytes");
    }

    buffer[got] = '\0';

    console_write("VFS test: rewind bytes: ");
    console_writeln(buffer);

    if (vfs_seek(file, 6, TOYIX_SEEK_SET, &pos) != VFS_OK || pos != 6u) {
        kernel_panic("VFS test could not seek to offset 6");
    }

    if (vfs_read(file, buffer, 5u, &got) != VFS_OK || got != 5u) {
        kernel_panic("VFS test could not read after seek");
    }

    buffer[got] = '\0';

    console_write("VFS test: seek bytes: ");
    console_writeln(buffer);

    vfs_close(file);

    console_writeln("VFS test: RAMFS stat/seek sanity check passed");
}
