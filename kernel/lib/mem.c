// kernel/lib/mem.c
#include <stddef.h>

void *memset(void * dest, int value, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    
    for (size_t i = 0; i < count; ++i) {
        d[i] = (unsigned char)value;
    }
    
    return dest;
}


void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    
    return dest;
}


void *memmove(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    
    if (d == s || count == 0) {
        return dest;
    }
    
    if (d < s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    
    return dest;
}


int memcmp(const void *left, const void * right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    
    return 0;
}









