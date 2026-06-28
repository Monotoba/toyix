#ifndef TOYIX_USER_SYSCALL_H
#define TOYIX_USER_SYSCALL_H

typedef unsigned int toyix_u32;
typedef int toyix_i32;

#define FD_STDIN  0u
#define FD_STDOUT 1u
#define FD_STDERR 2u

#define SYS_PUTC  1u
#define SYS_EXIT  2u
#define SYS_WRITE 3u
#define SYS_SLEEP 4u
#define SYS_READ  5u
#define SYS_EXEC  6u
#define SYS_WAITPID 7u
#define SYS_GETPID   8u
#define SYS_GETPPID  9u
#define SYS_PROCINFO 10u
#define SYS_KILL     11u
#define SYS_OPEN     12u
#define SYS_CLOSE    13u
#define SYS_SEEK     14u

#define TOYIX_SEEK_SET 0u
#define TOYIX_SEEK_CUR 1u
#define TOYIX_SEEK_END 2u

#define TOYIX_PROCESS_NEW       0u
#define TOYIX_PROCESS_RUNNING   1u
#define TOYIX_PROCESS_ZOMBIE    2u
#define TOYIX_PROCESS_DESTROYED 3u

typedef struct toyix_procinfo {
    toyix_u32 pid;
    toyix_u32 parent_pid;
    toyix_u32 state;
    toyix_u32 exit_code;
    toyix_u32 exited;
    toyix_u32 kill_requested;
} toyix_procinfo_t;

static inline toyix_i32 toyix_read(
    toyix_u32 fd,
    void *buffer,
    toyix_u32 length
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_READ),
          "b"(fd),
          "c"(buffer),
          "d"(length)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_write(
    toyix_u32 fd,
    const void *buffer,
    toyix_u32 length
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_WRITE),
          "b"(fd),
          "c"(buffer),
          "d"(length)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_sleep(toyix_u32 ticks) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_SLEEP),
          "b"(ticks)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_exec(
    const char *name,
    const char **argv,
    toyix_u32 argc
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_EXEC),
          "b"(name),
          "c"(argv),
          "d"(argc)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_waitpid(
    toyix_u32 pid,
    toyix_u32 *status
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_WAITPID),
          "b"(pid),
          "c"(status)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_getpid(void) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_GETPID)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_getppid(void) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_GETPPID)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_procinfo(
    toyix_u32 pid,
    toyix_procinfo_t *info
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_PROCINFO),
          "b"(pid),
          "c"(info)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_kill(toyix_u32 pid) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_KILL),
          "b"(pid)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_open(const char *path, toyix_u32 flags) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_OPEN),
          "b"(path),
          "c"(flags)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_close(toyix_u32 fd) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_CLOSE),
          "b"(fd)
        : "memory"
    );

    return result;
}

static inline toyix_i32 toyix_seek(
    toyix_u32 fd,
    toyix_i32 offset,
    toyix_u32 whence
) {
    toyix_i32 result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(SYS_SEEK),
          "b"(fd),
          "c"(offset),
          "d"(whence)
        : "memory"
    );

    return result;
}

static inline void toyix_exit(toyix_u32 code) {
    __asm__ volatile (
        "int $0x80"
        :
        : "a"(SYS_EXIT),
          "b"(code)
        : "memory"
    );

    for (;;) {
        __asm__ volatile ("jmp .");
    }
}

#endif
