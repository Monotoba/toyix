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
        .data = readme_text,
        .size = sizeof(readme_text) - 1u
    },
    {
        .path = "/programs",
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

void vfs_close(vfs_file_t *file) {
    if (file == 0) {
        return;
    }

    kfree(file);
}

void vfs_test_once(void) {
    console_writeln("VFS test: starting RAMFS open/read/close test");

    vfs_file_t *file = 0;

    if (vfs_open("/README", &file) != VFS_OK || file == 0) {
        kernel_panic("VFS test could not open /README");
    }

    char buffer[16];
    uint32_t got = 0;

    if (vfs_read(file, buffer, sizeof(buffer) - 1u, &got) != VFS_OK) {
        kernel_panic("VFS test could not read /README");
    }

    buffer[got] = '\0';

    console_write("VFS test: first bytes: ");
    console_writeln(buffer);

    vfs_close(file);

    console_writeln("VFS test: RAMFS sanity check passed");
}
