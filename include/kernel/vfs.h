// include/kernel/vfs.h
#ifndef TOYIX_KERNEL_VFS_H
#define TOYIX_KERNEL_VFS_H

#include <stdint.h>

#define VFS_OK 0
#define VFS_ERR_NOT_FOUND     -1
#define VFS_ERR_INVALID       -2
#define VFS_ERR_NO_MEMORY     -3
#define VFS_ERR_NOT_SUPPORTED -4

typedef struct vfs_file vfs_file_t;

void vfs_init(void);

int vfs_open(const char *path, vfs_file_t **out_file);
int vfs_read(
    vfs_file_t *file,
    void *buffer,
    uint32_t length,
    uint32_t *out_read
);
void vfs_close(vfs_file_t *file);

void vfs_test_once(void);

#endif
