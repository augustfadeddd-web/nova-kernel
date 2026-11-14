#ifndef NOVA_LIBC_ASSERT_H
#define NOVA_LIBC_ASSERT_H

/* Nova: assertion failures log to serial and abort().  DOOM never fires
 * NDEBUG-off asserts that we do not handle; abort() longjmps out. */
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
void __assert_fail(const char *expr, const char *file, int line)
    __attribute__((noreturn));
#endif

#endif
