/*
 * The C library functions urlmagic4 and PCRE2 use, for the NT 4.0 build (we link without a
 * C runtime). The heap is the process heap: in the filter that is IIS's own.
 */
#include <stddef.h>
#include <windows.h>

/* x86 string instructions: 4 bytes per step (PCRE2 copies a frame for every backtracking
   step, so these are hot). Windows guarantees the direction flag is clear. */
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    size_t w = n >> 2;
    size_t b = n & 3;
    unsigned v = (unsigned char)c * 0x01010101u;
    __asm__ volatile ("rep stosl" : "+D"(d), "+c"(w) : "a"(v) : "memory");
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(b) : "a"(v) : "memory");
    return r;
}

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    size_t w = n >> 2;
    size_t b = n & 3;
    __asm__ volatile ("rep movsl" : "+D"(d), "+S"(s), "+c"(w) : : "memory");
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(b) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) {
        while (n--) *p++ = *q++;
    } else {
        p += n;
        q += n;
        while (n--) *--p = *--q;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a;
    const unsigned char *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;   /* NOSONAR: ISO C's memchr returns void * */
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;    /* NOSONAR: ISO C's strchr returns char * */
        if (!*s) return NULL;
    }
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

void *malloc(size_t n)
{
    return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}

void free(void *p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}
