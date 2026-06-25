// include/kernel/string.h
#ifndef TOYIX_KERNEL_STRING_H
#define TOYIX_KERNEL_STRING_H

#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *left, const void *right, size_t count);

size_t kstrlen(const char *text);
int kstrcmp(const char *left, const char *right);
int kstrncmp(const char *left, const char *right, size_t count);
char *kstrcpy(char *dest, const char *src);

#endif
