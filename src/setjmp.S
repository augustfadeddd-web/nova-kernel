/* x86_64 System V setjmp/longjmp plus Nova's DOOM stack trampoline.
 * jmp_buf layout (eight 64-bit words): rbx, rbp, r12, r13, r14, r15, rsp, pc.
 * The callee-saved registers are rbx/rbp/r12-r15; rsp is the caller's
 * stack pointer taken just after setjmp's return address (so it still points
 * at the return address when longjmp jumps back). */

    .text
    .code64

    .globl setjmp
    .type setjmp, @function
setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax          /* caller's rsp: skip the return address */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax           /* return address */
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

    .globl longjmp
    .type longjmp, @function
longjmp:
    testl %esi, %esi
    jnz 1f
    movl $1, %esi               /* longjmp(buf, 0) resumes as setjmp == 1 */
1:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rcx
    movl %esi, %eax
    jmp *%rcx

    /* void nova_call_on_stack(void *stack_top, void (*fn)(void))
     * rbx is callee-saved: it holds the saved original rsp (whose slot also
     * anchors the old stack) until we return.  The callee sees an initial
     * rsp%16 == 8, exactly as after a normal call. */
    .globl nova_call_on_stack
    .type nova_call_on_stack, @function
nova_call_on_stack:
    pushq %rbx
    movq %rsp, %rbx
    andq $-16, %rdi             /* 16-byte alignment for the callee */
    movq %rdi, %rsp
    pushq $nova_frame_returned  /* fn's return address */
    jmp *%rsi
nova_frame_returned:
    movq %rbx, %rsp
    popq %rbx
    ret

    .section .note.GNU-stack,"",@progbits
