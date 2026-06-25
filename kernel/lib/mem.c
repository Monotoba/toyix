// kernel/lib/mem.c
#include <stddef.h>
#include "kernel/string.h"

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


int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    
    return 0;
}


size_t kstrlen(const char *text) {
    size_t length = 0;

    if (text == 0) {
        return 0;
    }

    while (text[length] != '\0') {
        length++;
    }

    return length;
}


int kstrcmp(const char *left, const char *right) {
    if (left == 0 && right == 0) {
        return 0;
    }

    if (left == 0) {
        return -1;
    }

    if (right == 0) {
        return 1;
    }

    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return (unsigned char)*left - (unsigned char)*right;
        }

        left++;
        right++;
    }

    return (unsigned char)*left - (unsigned char)*right;
}


int kstrncmp(const char *left, const char *right, size_t count) {
    if (count == 0) {
        return 0;
    }

    if (left == 0 && right == 0) {
        return 0;
    }

    if (left == 0) {
        return -1;
    }

    if (right == 0) {
        return 1;
    }

    for (size_t i = 0; i < count; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];

        if (a != b) {
            return (int)a - (int)b;
        }

        if (a == '\0') {
            return 0;
        }
    }

    return 0;
}


char *kstrcpy(char *dest, const char *src) {
    char *original = dest;

    if (dest == 0) {
        return 0;
    }

    if (src == 0) {
        dest[0] = '\0';
        return dest;
    }

    while (*src != '\0') {
        *dest++ = *src++;
    }

    *dest = '\0';

    return original;
}







