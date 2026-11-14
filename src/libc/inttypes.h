#ifndef NOVA_LIBC_INTTYPES_H
#define NOVA_LIBC_INTTYPES_H

#include <stdint.h>

/* The format macros DOOM's headers expect from C99's <inttypes.h>. */
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"
#define PRIX32 "X"
#define PRIX64 "llX"
#define PRIo8  "o"
#define PRIo32 "o"

#endif
