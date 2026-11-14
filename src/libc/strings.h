#ifndef NOVA_LIBC_STRINGS_H
#define NOVA_LIBC_STRINGS_H

#include <stddef.h>

/* doomtype.h pulls this in for the case-insensitive compare helpers. */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
