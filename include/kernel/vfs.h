// include/kernel/vfs.h
#ifndef TOYIX_KERNEL_VFS_H
#define TOYIX_KERNEL_VFS_H

#include <stdint.h>

#define VFS_OK 0
#define VFS_ERR_NOT_FOUND     -1
#define VFS_ERR_INVALID       -2
#define VFS_ERR_NO_MEMORY     -3
#define VFS_ERR_NOT_SUPPORTED -4

#define TOYIX_SEEK_SET 0u
#define TOYIX_SEEK_CUR 1u
#define TOYIX_SEEK_END 2u

#define VFS_NODE_REGULAR   1u
#define VFS_NODE_DIRECTORY 2u

typedef struct vfs_file vfs_file_t;

typedef struct vfs_stat {
    uint32_t type;
    uint32_t size;
} vfs_stat_t;

void vfs_init(void);

int vfs_open(const char *path, vfs_file_t **out_file);
int vfs_read(
    vfs_file_t *file,
    void *buffer,
    uint32_t length,
    uint32_t *out_read
);
int vfs_seek(
    vfs_file_t *file,
    int32_t offset,
    uint32_t whence,
    uint32_t *out_position
);
uint32_t vfs_tell(vfs_file_t *file);
uint32_t vfs_size(vfs_file_t *file);
int vfs_stat(const char *path, vfs_stat_t *out_stat);
void vfs_close(vfs_file_t *file);

void vfs_test_once(void);

#endif
