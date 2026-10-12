/* Compiler-emitted freestanding runtime operations. No allocation or locking. */
#include <stddef.h>
void *memset(void *destination, int value, size_t count)
{
    unsigned char *p=destination;
    for (size_t i=0;i<count;++i) p[i]=(unsigned char)value;
    return destination;
}
void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *d=destination; const unsigned char *s=source;
    for (size_t i=0;i<count;++i) d[i]=s[i];
    return destination;
}
