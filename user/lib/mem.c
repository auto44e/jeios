/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    for (unsigned long i = 0ul; i < n; ++i) out[i] = in[i];
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *out = (unsigned char *)dst;
    unsigned char value = (unsigned char)c;
    for (unsigned long i = 0ul; i < n; ++i) out[i] = value;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    if (out < in) {
        for (unsigned long i = 0ul; i < n; ++i) out[i] = in[i];
    } else if (out > in) {
        for (unsigned long i = n; i != 0ul; --i) out[i - 1ul] = in[i - 1ul];
    }
    return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *left = (const unsigned char *)a;
    const unsigned char *right = (const unsigned char *)b;
    for (unsigned long i = 0ul; i < n; ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}
