#ifndef NOVA_SETJMP_H
#define NOVA_SETJMP_H

/* x86_64 System V layout: rbx, rbp, r12, r13, r14, r15, rsp, pc — eight
 * 64-bit words (kept as int-sized elements so &env[0] is the stored rsp). */
typedef unsigned long jmp_buf[8];

/* setjmp runs at the top of DOOM's invoke() frame on the kernel stack; the
 * stack switch happens afterwards, so longjmp always lands on live frames. */
int setjmp(jmp_buf environment) __attribute__((returns_twice));
void longjmp(jmp_buf environment, int value) __attribute__((noreturn));

/* Runs fn() on the given stack top (16-byte aligned before the call).  If fn
 * longjmps away, the abandoned frame is never returned to. */
void nova_call_on_stack(void *stack_top, void (*fn)(void));

#endif
