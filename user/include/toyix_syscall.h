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
