#ifndef NOVA_LIBC_STDLIB_H
#define NOVA_LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

int abs(int value);
long labs(long value);
int rand(void);
void srand(unsigned int seed);

char *getenv(const char *name);
int system(const char *command);

/* Nova hook: DOOM's quit/error paths call exit(); the platform layer turns
 * that into a longjmp back out of the game loop. */
typedef void (*nova_exit_hook_t)(int status);
void nova_set_exit_hook(nova_exit_hook_t hook);

#endif
