/*
 * minilibc.c — the C library Nova OS links against.
 *
 * printf-family output goes straight to the serial port (terminal_log) so the
 * boot test can watch DOOM talk without touching the frame buffer.  The FILE
 * layer serves the embedded shareware WAD read-only, routes everything else
 * to the FAT32 volume on the ATA disk when one is mounted (real persistent
 * storage), and otherwise falls back to a discard stream so DOOM still runs
 * without a disk.  Memory comes from one address-ordered free list carved out
 * of a static arena.
 */
#include "terminal.h"
#include "fat32.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int errno = 0;

/* ------------------------------------------------------------------ heap -- */

enum { HEAP_ALIGN = 16, HEAP_SIZE = 48u * 1024u * 1024u };

struct block {
    size_t size; /* payload bytes following the header */
    bool used;
    struct block *next, *prev;
};

static unsigned char arena[HEAP_SIZE] __attribute__((aligned(16)));
static struct block *heap_head;
static bool heap_ready;

static void heap_boot(void)
{
    heap_head = (struct block *)arena;
    heap_head->size = HEAP_SIZE - sizeof(struct block);
    heap_head->used = false;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_ready = true;
}

void *malloc(size_t size)
{
    struct block *block;
    if (!heap_ready) heap_boot();
    if (size > HEAP_SIZE - sizeof(struct block) - HEAP_ALIGN) return NULL;
    if (!size) size = 1;
    size = (size + (HEAP_ALIGN - 1)) & ~(size_t)(HEAP_ALIGN - 1);
    for (block = heap_head; block; block = block->next) {
        struct block *rest;
        if (block->used || block->size < size) continue;
        if (block->size >= size + sizeof(struct block) + HEAP_ALIGN) {
            rest = (struct block *)((unsigned char *)(block + 1) + size);
            rest->size = block->size - size - sizeof(struct block);
            rest->used = false;
            rest->next = block->next;
            rest->prev = block;
            if (rest->next) rest->next->prev = rest;
            block->next = rest;
            block->size = size;
        }
        block->used = true;
        return block + 1;
    }
    return NULL;
}

static struct block *heap_find(void *pointer)
{
    if (!heap_ready || (uintptr_t)pointer < (uintptr_t)arena + sizeof(struct block) ||
        (uintptr_t)pointer >= (uintptr_t)arena + HEAP_SIZE) return NULL;
    for (struct block *b = heap_head; b; b = b->next)
        if ((void *)(b + 1) == pointer) return b->used ? b : NULL;
    return NULL;
}

void free(void *pointer)
{
    struct block *block;
    if (!pointer) return;
    block = heap_find(pointer);
    if (!block) return;
    block->used = false;
    if (block->next && !block->next->used) {
        block->size += sizeof(struct block) + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }
    if (block->prev && !block->prev->used) {
        block->prev->size += sizeof(struct block) + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}

void *calloc(size_t count, size_t size)
{
    size_t total;
    void *memory;
    if (size && count > (size_t)-1 / size) return NULL;
    total = count * size;
    memory = malloc(total);
    if (memory) memset(memory, 0, total);
    return memory;
}

void *realloc(void *pointer, size_t size)
{
    struct block *block;
    void *fresh;
    size_t old;
    if (!pointer) return malloc(size);
    if (!size) { free(pointer); return NULL; }
    block = heap_find(pointer);
    if (!block) return NULL;
    old = block->size;
    if (old >= size) return pointer;
    fresh = malloc(size);
    if (!fresh) return NULL;
    memcpy(fresh, pointer, old);
    free(pointer);
    return fresh;
}

/* -------------------------------------------------------- process control -- */

static nova_exit_hook_t exit_hook;

void nova_set_exit_hook(nova_exit_hook_t hook)
{
    exit_hook = hook;
}

void exit(int status)
{
    if (exit_hook) exit_hook(status); /* longjmps out of DOOM when armed */
    terminal_log("DOOM_EXIT_STALE\n");
    for (;;) { }
}

void abort(void)
{
    if (exit_hook) exit_hook(1);
    terminal_log("DOOM_ABORT_STALE\n");
    for (;;) { }
}

void __assert_fail(const char *expr, const char *file, int line)
{
    char location[96];
    snprintf(location, sizeof(location), "ASSERT: %s at %s:%d\n", expr, file, line);
    terminal_log(location);
    abort();
}

/* ------------------------------------------------------------------ mem ---- */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || !n) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        ++x;
        ++y;
    }
    return 0;
}

void *memchr(const void *ptr, int value, size_t n)
{
    const unsigned char *p = ptr;
    while (n--) {
        if (*p == (unsigned char)value) return (void *)p;
        ++p;
    }
    return NULL;
}

/* --------------------------------------------------------------- strings --- */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t length = 0;
    while (length < max && s[length]) ++length;
    return length;
}

char *strcpy(char *dst, const char *src)
{
    char *result = dst;
    while ((*dst++ = *src++) != 0) { }
    return result;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *result = dst;
    while (n && *src) { *dst++ = *src++; --n; }
    while (n--) *dst++ = 0;
    return result;
}

char *strcat(char *dst, const char *src)
{
    char *result = dst;
    while (*dst) ++dst;
    while ((*dst++ = *src++) != 0) { }
    return result;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *result = dst;
    while (*dst) ++dst;
    while (n-- && *src) *dst++ = *src++;
    *dst = 0;
    return result;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { ++a; ++b; --n; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    char wanted = (char)c;
    for (;; ++s) {
        if (*s == wanted) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    char wanted = (char)c;
    const char *last = NULL;
    for (;; ++s) {
        if (*s == wanted) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t length = strlen(needle);
    if (!length) return (char *)haystack;
    for (; *haystack; ++haystack) {
        if (*haystack == *needle && !strncmp(haystack, needle, length))
            return (char *)haystack;
    }
    return NULL;
}

int strcasecmp(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        int x = tolower((unsigned char)*a);
        int y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        int x = tolower((unsigned char)*a);
        int y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
        ++a;
        ++b;
    }
    return 0;
}

char *strdup(const char *s)
{
    size_t length = strlen(s) + 1;
    char *copy = malloc(length);
    if (copy) memcpy(copy, s, length);
    return copy;
}

char *strerror(int errnum)
{
    (void)errnum;
    return "error";
}

/* ---------------------------------------------------------------- ctype ---- */

int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r';
}
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 32 && c < 127; }
int ispunct(int c)
{
    return isprint(c) && !isalnum(c) && !isspace(c);
}
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

/* ----------------------------------------------------------------- math ---- */

double fabs(double x)
{
    union { double d; uint64_t u; } value;
    value.d = x;
    value.u &= 0x7FFFFFFFFFFFFFFFull;
    return value.d;
}

double sqrt(double x)
{
    double result;
    __asm__ volatile ("fsqrt" : "=t"(result) : "0"(x));
    return result;
}

double floor(double x)
{
    double truncated;
    if (!(x >= -2147483648.0 && x < 2147483648.0)) return x;
    truncated = (double)(long)x;
    return truncated > x ? truncated - 1.0 : truncated;
}

double ceil(double x)
{
    double truncated;
    if (!(x >= -2147483648.0 && x < 2147483648.0)) return x;
    truncated = (double)(long)x;
    return truncated < x ? truncated + 1.0 : truncated;
}

/* Range-reduce into [-2*pi, 2*pi] using one hardware division; the series
 * below only ever sees a small argument. */
static double reduce_angle(double x)
{
    double turns = x / (2.0 * M_PI);
    return x - floor(turns) * (2.0 * M_PI);
}

static double sine_series(double x)
{
    double x2 = x * x;
    double term = x;
    double sum = x;
    int n;
    for (n = 1; n <= 11; ++n) {
        term *= -x2;
        term /= (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double sin(double x)
{
    x = reduce_angle(x);
    if (x > M_PI) x -= 2.0 * M_PI;
    if (x < -M_PI) x += 2.0 * M_PI;
    if (x > M_PI / 2.0) x = M_PI - x;
    if (x < -M_PI / 2.0) x = -M_PI - x;
    return sine_series(x);
}

double cos(double x)
{
    return sin(x + M_PI / 2.0);
}

double tan(double x)
{
    double cosine = cos(x);
    if (cosine == 0.0) return x < 0.0 ? -1e30 : 1e30;
    return sin(x) / cosine;
}

/* atan on [-1,1] by Taylor; callers reduce first. */
static double atan_series(double x)
{
    double x2 = x * x;
    double term = x;
    double sum = x;
    int n;
    for (n = 1; n <= 15; ++n) {
        term *= -x2;
        sum += term / (double)(2 * n + 1);
    }
    return sum;
}

double atan(double x)
{
    int invert = 0;
    int halvings = 0;
    double result;
    if (x != x) return x; /* NaN */
    if (x > 1.0) { x = 1.0 / x; invert = 1; }
    else if (x < -1.0) { x = 1.0 / x; invert = 1; }
    while (x > 0.4142135623 || x < -0.4142135623) {
        x = x / (1.0 + sqrt(1.0 + x * x));
        if (++halvings > 3) break;
    }
    result = atan_series(x);
    while (halvings--) result *= 2.0;
    if (invert) result = (x >= 0.0 ? M_PI / 2.0 : -M_PI / 2.0) - result;
    return result;
}

double atan2(double y, double x)
{
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return atan(y / x) + M_PI;
        return atan(y / x) - M_PI;
    }
    /* x == 0 */
    if (y > 0.0) return M_PI / 2.0;
    if (y < 0.0) return -M_PI / 2.0;
    return 0.0;
}

/* ---------------------------------------------------------- output sink --- */

struct sink {
    char chunk[160];
    unsigned int used;
    char *buffer;
    size_t capacity;
    size_t total;
    bool serial;
};

static void sink_flush(struct sink *s)
{
    if (s->serial && s->used) {
        s->chunk[s->used] = 0;
        terminal_log(s->chunk);
    }
    s->used = 0;
}

static void sink_put(struct sink *s, char c)
{
    if (s->buffer) {
        if (s->total + 1 < s->capacity) s->buffer[s->total] = c;
        ++s->total;
        return;
    }
    if (!s->serial) { ++s->total; return; }
    if (s->used == sizeof(s->chunk) - 1) sink_flush(s);
    s->chunk[s->used++] = c;
    ++s->total;
}

static void sink_spaces(struct sink *s, char pad, int count)
{
    while (count-- > 0) sink_put(s, pad);
}

/* Writes an unsigned magnitude, honouring sign, precision and width. */
static void sink_number(struct sink *s, unsigned long long magnitude,
                        unsigned int base, int upper, int negative, int width,
                        char pad, int left, int precision)
{
    char digits[64];
    int count = 0;
    int zeros = 0;
    int spaces = 0;
    int total;

    if (base < 2 || base > 16) base = 10;
    do {
        unsigned int digit;
        if (magnitude <= 0xFFFFFFFFull) {
            unsigned int value32 = (unsigned int)magnitude;
            digit = value32 % base;
            magnitude = value32 / base;
        } else {
            digit = (unsigned int)(magnitude % base);
            magnitude /= base;
        }
        digits[count++] = (char)(digit < 10 ? '0' + digit
                                            : (upper ? 'A' : 'a') + digit - 10);
    } while (magnitude);

    if (precision > count) zeros = precision - count;
    total = count + zeros + (negative ? 1 : 0);
    if (width > total) {
        if (!left && pad == '0') zeros += width - total;
        else spaces = width - total;
    }
    if (negative) sink_put(s, '-');
    sink_spaces(s, '0', zeros);
    while (count) sink_put(s, digits[--count]);
    sink_spaces(s, ' ', spaces);
}

static void sink_format(struct sink *s, const char *format, va_list args)
{
    while (*format) {
        int width = 0;
        int precision = -1;
        int left = 0;
        int longs = 0;
        char pad = ' ';
        char conv;

        if (*format != '%') {
            sink_put(s, *format++);
            continue;
        }
        ++format;
        for (;;) {
            if (*format == '-') { left = 1; ++format; }
            else if (*format == '0') { pad = '0'; ++format; }
            else if (*format == ' ' || *format == '+') ++format;
            else break;
        }
        while (isdigit((unsigned char)*format)) {
            width = width * 10 + (*format - '0');
            ++format;
        }
        if (*format == '.') {
            ++format;
            precision = 0;
            while (isdigit((unsigned char)*format)) {
                precision = precision * 10 + (*format - '0');
                ++format;
            }
        }
        while (*format == 'l') { ++longs; ++format; }
        while (*format == 'h' || *format == 'z' || *format == 't') ++format;
        conv = *format;
        if (!conv) break;
        ++format;

        switch (conv) {
        case '%':
            sink_put(s, '%');
            break;
        case 'c': {
            char text[2];
            int length = 1;
            text[0] = (char)va_arg(args, int);
            text[1] = 0;
            if (!left) sink_spaces(s, ' ', width - length);
            sink_put(s, text[0]);
            if (left) sink_spaces(s, ' ', width - length);
            break;
        }
        case 's': {
            const char *text = va_arg(args, const char *);
            int length;
            int padding;
            if (!text) text = "(null)";
            length = (int)(precision >= 0 ? strnlen(text, (size_t)precision)
                                          : strlen(text));
            padding = width > length ? width - length : 0;
            if (!left) sink_spaces(s, ' ', padding);
            while (length-- > 0) sink_put(s, *text++);
            if (left) sink_spaces(s, ' ', padding);
            break;
        }
        case 'd':
        case 'i': {
            unsigned long long magnitude;
            int negative;
            long long signed_value;
            if (longs >= 2) signed_value = va_arg(args, long long);
            else if (longs == 1) signed_value = (long long)va_arg(args, long);
            else signed_value = (long long)va_arg(args, int);
            negative = signed_value < 0;
            magnitude = negative
                ? (unsigned long long)(-signed_value)
                : (unsigned long long)signed_value;
            sink_number(s, magnitude, 10, 0, negative, width, pad, left,
                        precision);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            unsigned long long value;
            unsigned int base;
            int upper = 0;
            int negative = 0;
            if (conv == 'p') {
                value = (unsigned long long)(uintptr_t)va_arg(args, void *);
                base = 16;
            } else if (longs >= 2) value = va_arg(args, unsigned long long);
            else if (longs == 1) value = (unsigned long long)va_arg(args, unsigned long);
            else value = (unsigned long long)va_arg(args, unsigned int);
            if (conv == 'o') base = 8;
            else if (conv == 'u') base = 10;
            else { base = 16; upper = conv == 'X'; }
            if (conv == 'p') {
                if (width <= 2) width = 0;
                else width -= 2;
                sink_put(s, '0');
                sink_put(s, 'x');
            }
            sink_number(s, value, base, upper, negative, width, pad, left,
                        precision);
            break;
        }
        default:
            sink_put(s, '%');
            sink_put(s, conv);
            break;
        }
    }
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    struct sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.buffer = buffer;
    sink.capacity = size;
    sink_format(&sink, format, args);
    if (buffer && size)
        buffer[sink.total < size ? sink.total : size - 1] = 0;
    return (int)sink.total;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int result;
    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

int sprintf(char *buffer, const char *format, ...)
{
    struct sink sink;
    va_list args;
    int result;
    memset(&sink, 0, sizeof(sink));
    sink.buffer = buffer;
    sink.capacity = (size_t)-1;
    va_start(args, format);
    sink_format(&sink, format, args);
    va_end(args);
    buffer[sink.total] = 0;
    result = (int)sink.total;
    return result;
}

/* Everything printed while DOOM runs goes to the serial console only, so the
 * boot test can read it and the desktop never sees it. */
static int serial_format(const char *format, va_list args)
{
    struct sink sink;
    int result;
    memset(&sink, 0, sizeof(sink));
    sink.serial = true;
    sink_format(&sink, format, args);
    sink_flush(&sink);
    result = (int)sink.total;
    return result;
}

int printf(const char *format, ...)
{
    va_list args;
    int result;
    va_start(args, format);
    result = serial_format(format, args);
    va_end(args);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list args)
{
    struct sink sink;
    int result;
    memset(&sink, 0, sizeof(sink));
    sink.serial = stream == NULL || stream == stdout || stream == stderr ||
                  stream == stdin;
    sink_format(&sink, format, args);
    sink_flush(&sink);
    result = (int)sink.total;
    return result;
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    int result;
    va_start(args, format);
    result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int puts(const char *s)
{
    terminal_log(s);
    terminal_log("\n");
    return 0;
}

int putchar(int c)
{
    char text[2];
    text[0] = (char)c;
    text[1] = 0;
    terminal_log(text);
    return c;
}

void perror(const char *s)
{
    fprintf(stderr, "%s: %s\n", s ? s : "error", strerror(errno));
}

/* ----------------------------------------------------------- FILE layer --- */

enum { KIND_UNUSED = 0, KIND_WAD, KIND_DISCARD, KIND_FAT, KIND_COUNT };

struct FILE {
    int kind;
    const unsigned char *data;
    void *backend; /* fat32_file for KIND_FAT */
    size_t size;
    size_t position;
    int pushback;
    bool eof;
};

#ifdef NOVA_DOOM_APP
/* The shareware IWAD lives in the independent DOOM ELF. */
extern const unsigned char doom1_wad_start[];
extern const unsigned char doom1_wad_end[];
#endif

static struct FILE pool[8];
static struct FILE standard[3];
FILE *stdin = &standard[0];
FILE *stdout = &standard[1];
FILE *stderr = &standard[2];

#ifdef NOVA_DOOM_APP
static bool wad_path(const char *path)
{
    static const char suffix[] = "doom1.wad";
    size_t length = strlen(path);
    size_t suffix_length = sizeof(suffix) - 1;
    if (length < suffix_length) return false;
    return strncasecmp(path + length - suffix_length, suffix,
                       suffix_length) == 0;
}

#endif
FILE *fopen(const char *path, const char *mode)
{
    int i;
    if (!path || !mode || !mode[0]) return NULL;
#ifdef NOVA_DOOM_APP
    /* The embedded shareware IWAD is read-only and always available. */
    if (mode[0] == 'r' && wad_path(path)) {
        for (i = 0; i < 8; ++i) {
            struct FILE *file = &pool[i];
            if (file->kind != KIND_UNUSED) continue;
            file->kind = KIND_WAD;
            file->data = doom1_wad_start;
            file->backend = NULL;
            file->size = (size_t)(doom1_wad_end - doom1_wad_start);
            file->position = 0;
            file->pushback = -1;
            file->eof = false;
            return file;
        }
        return NULL;
    }
#endif
    /* Everything else goes to the FAT32 volume when a disk is mounted so
     * DOOM's config/savegames (and Nova's own files) persist for real. */
    if (fat32_mounted()) {
        void *backend = fat32_open(path, mode);
        if (!backend) return NULL; /* errno already set (e.g. EISDIR). */
        for (i = 0; i < 8; ++i) {
            struct FILE *file = &pool[i];
            if (file->kind != KIND_UNUSED) continue;
            file->kind = KIND_FAT;
            file->data = NULL;
            file->backend = backend;
            file->size = fat32_length(backend);
            file->position = (size_t)fat32_tell(backend);
            file->pushback = -1;
            file->eof = false;
            return file;
        }
        fat32_close(backend);
        return NULL;
    }
    /* No disk: reads of non-WAD files fail; writes are discarded so DOOM
     * still runs (its config/save paths degrade gracefully). */
    if (mode[0] == 'r') return NULL;
    if (mode[0] == 'w' || mode[0] == 'a') {
        for (i = 0; i < 8; ++i) {
            struct FILE *file = &pool[i];
            if (file->kind != KIND_UNUSED) continue;
            file->kind = KIND_DISCARD;
            file->data = NULL;
            file->backend = NULL;
            file->size = 0;
            file->position = 0;
            file->pushback = -1;
            file->eof = false;
            return file;
        }
    }
    return NULL;
}

int fclose(FILE *stream)
{
    int i;
    if (!stream) return -1;
    for (i = 0; i < 8; ++i)
        if (stream == &pool[i]) {
            if (stream->kind == KIND_FAT) fat32_close(stream->backend);
            stream->kind = KIND_UNUSED;
            stream->backend = NULL;
            return 0;
        }
    return 0;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream)
{
    size_t bytes, available, taken;
    if (!stream || !ptr || !size || !count) return 0;
    bytes = size * count;
    if (stream->kind == KIND_FAT) {
        size_t items = fat32_read(stream->backend, ptr, size, count);
        stream->position = (size_t)fat32_tell(stream->backend);
        stream->size = fat32_length(stream->backend);
        if (items < count) stream->eof = true;
        return items;
    }
    if (stream->kind != KIND_WAD) return 0;
    available = stream->position < stream->size
              ? stream->size - stream->position : 0;
    taken = bytes < available ? bytes : available;
    memcpy(ptr, stream->data + stream->position, taken);
    stream->position += taken;
    if (taken < bytes) stream->eof = true;
    return taken / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    if (!stream || !ptr || !size || !count) return 0;
    if (stream->kind == KIND_WAD) return 0; /* read-only */
    if (stream->kind == KIND_FAT) {
        size_t items = fat32_write(stream->backend, ptr, size, count);
        stream->position = (size_t)fat32_tell(stream->backend);
        stream->size = fat32_length(stream->backend);
        stream->eof = false;
        return items;
    }
    size_t bytes = size * count;
    stream->position += bytes;
    if (stream->position > stream->size) stream->size = stream->position;
    return count;
}

int fseek(FILE *stream, long offset, int whence)
{
    long base, target;
    if (!stream) return -1;
    if (stream->kind == KIND_FAT) {
        if (fat32_seek(stream->backend, offset, whence) != 0) return -1;
        stream->position = (size_t)fat32_tell(stream->backend);
        stream->size = fat32_length(stream->backend);
        stream->pushback = -1;
        stream->eof = false;
        return 0;
    }
    if (whence == SEEK_CUR) base = (long)stream->position;
    else if (whence == SEEK_END) base = (long)stream->size;
    else if (whence == SEEK_SET) base = 0;
    else return -1;
    target = base + offset;
    if (target < 0) return -1;
    stream->position = (size_t)target;
    stream->pushback = -1;
    stream->eof = false;
    return 0;
}

long ftell(FILE *stream)
{
    return stream ? (long)stream->position : -1L;
}

int feof(FILE *stream)
{
    if (!stream) return 0;
    if (stream->kind == KIND_WAD) return stream->position >= stream->size;
    if (stream->kind == KIND_FAT) return stream->position >= stream->size ? 1 : 0;
    return stream->eof ? 1 : 0;
}

int ferror(FILE *stream)
{
    (void)stream;
    return 0;
}

int fflush(FILE *stream)
{
    (void)stream;
    return 0;
}

int remove(const char *path)
{
    if (!path) return -1;
    if (fat32_mounted()) return fat32_remove(path);
    return 0; /* discard layer: nothing to remove */
}

int rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;
    if (fat32_mounted()) return fat32_rename(old_path, new_path);
    return 0;
}

/* --------------------------------------------------------- scanf subset --- */

struct scanner {
    const char *text;
    FILE *stream;
    int peeked;   /* -2 = nothing buffered */
    bool hit_eof;
};

static int scan_get(struct scanner *sc)
{
    if (sc->peeked != -2) {
        int c = sc->peeked;
        sc->peeked = -2;
        return c;
    }
    if (sc->stream) {
        FILE *file = sc->stream;
        if (file->pushback >= 0) {
            int c = file->pushback;
            file->pushback = -1;
            return c;
        }
        if (file->kind == KIND_WAD && file->position < file->size)
            return file->data[file->position++];
        sc->hit_eof = true;
        return -1;
    }
    if (*sc->text) return (unsigned char)*sc->text++;
    sc->hit_eof = true;
    return -1;
}

static void scan_unget(struct scanner *sc, int c)
{
    if (c != -1) sc->peeked = c;
}

/* Reads an optionally signed integer.  base == 0 means "detect" (0x/0/10). */
static int scan_int(struct scanner *sc, int base, int allow_sign,
                    unsigned long long *out)
{
    int c = scan_get(sc);
    int negative = 0;
    int digits = 0;
    int saw_zero = 0;
    unsigned long long value = 0;

    if (allow_sign && (c == '+' || c == '-')) {
        negative = c == '-';
        c = scan_get(sc);
    }
    if (base == 0) {
        base = 10;
        if (c == '0') {
            int next = scan_get(sc);
            if (next == 'x' || next == 'X') {
                base = 16;
                c = scan_get(sc);
            } else {
                base = 8;
                c = next;
                saw_zero = 1;
            }
        }
    }
    for (;;) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        value = value * (unsigned long long)base + (unsigned long long)digit;
        ++digits;
        c = scan_get(sc);
    }
    scan_unget(sc, c);
    if (!digits && !saw_zero) return 0;
    if (negative) value = (unsigned long long)(-(long long)value);
    *out = value;
    return 1;
}

static int scan_matches(const char *set, int negate, int c)
{
    return (set[c & 0xFF] != 0) != (negate != 0);
}

static int scan_format(struct scanner *sc, const char *format, va_list args)
{
    int assigned = 0;

    while (*format) {
        char conv;
        int width = 0;
        int suppress = 0;
        int longs = 0;

        if (isspace((unsigned char)*format)) {
            int c;
            do { c = scan_get(sc); }
            while (c != -1 && isspace(c));
            scan_unget(sc, c);
            ++format;
            continue;
        }
        if (*format != '%') {
            int c = scan_get(sc);
            if (c != (unsigned char)*format) {
                scan_unget(sc, c);
                return assigned ? assigned : (c == -1 ? -1 : 0);
            }
            ++format;
            continue;
        }
        ++format;
        if (*format == '*') { suppress = 1; ++format; }
        while (isdigit((unsigned char)*format)) {
            width = width * 10 + (*format - '0');
            ++format;
        }
        while (*format == 'l' || *format == 'h' || *format == 'z') {
            if (*format == 'l') ++longs;
            ++format;
        }
        conv = *format;
        if (!conv) break;
        ++format;

        if (conv == '%') {
            int c = scan_get(sc);
            if (c != '%') {
                scan_unget(sc, c);
                return assigned ? assigned : (c == -1 ? -1 : 0);
            }
            continue;
        }
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' ||
            conv == 'X' || conv == 'o') {
            int base = conv == 'd' || conv == 'u' ? 10
                     : conv == 'o' ? 8
                     : conv == 'i' ? 0 : 16;
            unsigned long long value;
            if (!scan_int(sc, base, conv == 'd' || conv == 'i', &value))
                return assigned ? assigned : (sc->hit_eof ? -1 : 0);
            if (!suppress) {
                if (longs >= 2) {
                    if (conv == 'd' || conv == 'i') {
                        long long *target = va_arg(args, long long *);
                        *target = (long long)value;
                    } else {
                        unsigned long long *target =
                            va_arg(args, unsigned long long *);
                        *target = value;
                    }
                } else if (longs == 1) {
                    if (conv == 'd' || conv == 'i') {
                        long *target = va_arg(args, long *);
                        *target = (long)value;
                    } else {
                        unsigned long *target = va_arg(args, unsigned long *);
                        *target = (unsigned long)value;
                    }
                } else if (conv == 'd' || conv == 'i') {
                    int *target = va_arg(args, int *);
                    *target = (int)value;
                } else {
                    unsigned int *target = va_arg(args, unsigned int *);
                    *target = (unsigned int)value;
                }
                ++assigned;
            }
            continue;
        }
        if (conv == 'c') {
            char *target = suppress ? NULL : va_arg(args, char *);
            int wanted = width > 0 ? width : 1;
            int read = 0;
            while (read < wanted) {
                int c = scan_get(sc);
                if (c == -1) break;
                if (target) target[read] = (char)c;
                ++read;
            }
            if (read == 0) return assigned ? assigned : -1;
            if (!suppress) ++assigned;
            continue;
        }
        if (conv == 's') {
            char *target = suppress ? NULL : va_arg(args, char *);
            int read = 0;
            int c;
            do { c = scan_get(sc); }
            while (c != -1 && isspace(c));
            while (c != -1 && !isspace(c)) {
                if (width && read >= width) { scan_unget(sc, c); break; }
                if (target) target[read] = (char)c;
                ++read;
                c = scan_get(sc);
            }
            scan_unget(sc, c);
            if (read == 0) return assigned ? assigned : (sc->hit_eof ? -1 : 0);
            if (target) target[read] = 0;
            if (!suppress) ++assigned;
            continue;
        }
        if (conv == '[') {
            char set[256];
            int negate = 0;
            char *target = suppress ? NULL : va_arg(args, char *);
            int read = 0;
            int c;
            memset(set, 0, sizeof(set));
            if (*format == '^') { negate = 1; ++format; }
            if (*format == ']') { set[']'] = 1; ++format; }
            while (*format && *format != ']') {
                set[(unsigned char)*format] = 1;
                ++format;
            }
            if (*format == ']') ++format;
            c = scan_get(sc);
            while (c != -1 && scan_matches(set, negate, c)) {
                if (width && read >= width) { scan_unget(sc, c); break; }
                if (target) target[read] = (char)c;
                ++read;
                c = scan_get(sc);
            }
            scan_unget(sc, c);
            if (read == 0) return assigned ? assigned : (sc->hit_eof ? -1 : 0);
            if (target) target[read] = 0;
            if (!suppress) ++assigned;
            continue;
        }
        /* Unsupported conversion: stop rather than corrupt the arguments. */
        return assigned;
    }
    return assigned;
}

int sscanf(const char *str, const char *format, ...)
{
    struct scanner scanner_state;
    va_list args;
    int result;
    if (!str || !format) return -1;
    scanner_state.text = str;
    scanner_state.stream = NULL;
    scanner_state.peeked = -2;
    scanner_state.hit_eof = false;
    va_start(args, format);
    result = scan_format(&scanner_state, format, args);
    va_end(args);
    return result;
}

int fscanf(FILE *stream, const char *format, ...)
{
    struct scanner scanner_state;
    va_list args;
    int result;
    if (!stream || !format) return -1;
    scanner_state.text = NULL;
    scanner_state.stream = stream;
    scanner_state.peeked = -2;
    scanner_state.hit_eof = false;
    va_start(args, format);
    result = scan_format(&scanner_state, format, args);
    va_end(args);
    return result;
}

/* ------------------------------------------------------------- stdlib ----- */

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

long strtol(const char *s, char **end, int base)
{
    unsigned long value = strtoul(s, end, base < 0 ? 10 : base);
    return (long)value;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *start = s;
    unsigned long value = 0;
    int any = 0;
    int negative = 0;
    while (isspace((unsigned char)*s)) ++s;
    if (*s == '+') ++s;
    else if (*s == '-') { negative = 1; ++s; }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    for (;;) {
        int digit;
        if (isdigit((unsigned char)*s)) digit = *s - '0';
        else if (isalpha((unsigned char)*s)) digit = tolower(*s) - 'a' + 10;
        else break;
        if (digit >= base) break;
        value = value * (unsigned long)base + (unsigned long)digit;
        ++s;
        any = 1;
    }
    if (!any) { if (end) *end = (char *)start; return 0; }
    if (end) *end = (char *)s;
    return negative ? 0ul - value : value;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

double atof(const char *s)
{
    double value = 0.0;
    double fraction = 0.0;
    double scale = 1.0;
    int negative = 0;
    int any = 0;
    while (isspace((unsigned char)*s)) ++s;
    if (*s == '+') ++s;
    else if (*s == '-') { negative = 1; ++s; }
    while (isdigit((unsigned char)*s)) {
        value = value * 10.0 + (double)(*s - '0');
        ++s;
        any = 1;
    }
    if (*s == '.') {
        ++s;
        while (isdigit((unsigned char)*s)) {
            scale /= 10.0;
            fraction += scale * (double)(*s - '0');
            ++s;
            any = 1;
        }
    }
    if (!any) return 0.0;
    value += fraction;
    return negative ? -value : value;
}

static unsigned long random_state = 0x2545F491ul;

int rand(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (int)(random_state & 0x7FFFFFFFul);
}

void srand(unsigned int seed)
{
    random_state = seed ? seed : 1ul;
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int system(const char *command)
{
    (void)command;
    return -1; /* no shell in Nova; Zenity probes fail harmlessly */
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    if (!path) return -1;
    if (fat32_mounted()) return fat32_mkdir(path) ? 0 : -1;
    return 0; /* discard layer: directories exist virtually */
}

/* ----------------------------------------------------------- posix stubs -- */

int open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return -1;
}

int close(int fd)
{
    (void)fd;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    return -1;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    if (fd == STDERR_FILENO || fd == STDOUT_FILENO) {
        const char *text = buf;
        size_t i;
        for (i = 0; i < count; ++i) putchar((unsigned char)text[i]);
        return (ssize_t)count;
    }
    return -1;
}

off_t lseek(int fd, off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;
}

int access(const char *path, int mode)
{
    (void)path;
    (void)mode;
    return -1;
}

int unlink(const char *path)
{
    (void)path;
    return -1;
}

int chdir(const char *path)
{
    (void)path;
    return -1;
}

char *getcwd(char *buf, size_t size)
{
    (void)buf;
    (void)size;
    return NULL;
}

unsigned int sleep(unsigned int seconds)
{
    (void)seconds;
    return 0;
}

int usleep(useconds_t usec)
{
    (void)usec;
    return 0;
}

int stat(const char *path, struct stat *buf)
{
    (void)path;
    (void)buf;
    return -1;
}

int fstat(int fd, struct stat *buf)
{
    (void)fd;
    (void)buf;
    return -1;
}
