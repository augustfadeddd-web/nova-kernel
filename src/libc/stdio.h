#ifndef NOVA_LIBC_STDIO_H
#define NOVA_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list args);
int sprintf(char *buffer, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int sscanf(const char *str, const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int puts(const char *s);
int putchar(int c);
void perror(const char *s);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fflush(FILE *stream);
int remove(const char *path);
int rename(const char *old_path, const char *new_path);

#endif
