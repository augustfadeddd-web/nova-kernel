.section .bss
.align 16
kernel_fx: .skip 512
kernel_resume: .skip 8
.section .text
.code64
.global arch_user_run
arch_user_run:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp,kernel_resume(%rip)
    fxsave64 kernel_fx(%rip)
    fxrstor64 (%rdx)
    mov %rsi,%cr3
    mov $0x1b,%ax
    mov %ax,%ds
    mov %ax,%es
    mov %rdi,%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16,%rsp
    iretq
trap_common:
    cld
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp,%rdi
    and $-16,%rsp
    sub $512,%rsp
    fxsave64 (%rsp)
    mov %rsp,%rsi
    call process_trap
    mov kernel_cr3(%rip),%rax
    mov %rax,%cr3
    mov $0x10,%ax
    mov %ax,%ds
    mov %ax,%es
    mov %ax,%ss
    fxrstor64 kernel_fx(%rip)
    mov kernel_resume(%rip),%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

.global trap_0
trap_0:
    pushq $0
    pushq $0
    jmp trap_common

.global trap_1
trap_1:
    pushq $0
    pushq $1
    jmp trap_common

.global trap_2
trap_2:
    pushq $0
    pushq $2
    jmp trap_common

.global trap_3
trap_3:
    pushq $0
    pushq $3
    jmp trap_common

.global trap_4
trap_4:
    pushq $0
    pushq $4
    jmp trap_common

.global trap_5
trap_5:
    pushq $0
    pushq $5
    jmp trap_common

.global trap_6
trap_6:
    pushq $0
    pushq $6
    jmp trap_common

.global trap_7
trap_7:
    pushq $0
    pushq $7
    jmp trap_common

.global trap_8
trap_8:
    pushq $8
    jmp trap_common

.global trap_9
trap_9:
    pushq $0
    pushq $9
    jmp trap_common

.global trap_10
trap_10:
    pushq $10
    jmp trap_common

.global trap_11
trap_11:
    pushq $11
    jmp trap_common

.global trap_12
trap_12:
    pushq $12
    jmp trap_common

.global trap_13
trap_13:
    pushq $13
    jmp trap_common

.global trap_14
trap_14:
    pushq $14
    jmp trap_common

.global trap_15
trap_15:
    pushq $0
    pushq $15
    jmp trap_common

.global trap_16
trap_16:
    pushq $0
    pushq $16
    jmp trap_common

.global trap_17
trap_17:
    pushq $17
    jmp trap_common

.global trap_18
trap_18:
    pushq $0
    pushq $18
    jmp trap_common

.global trap_19
trap_19:
    pushq $0
    pushq $19
    jmp trap_common

.global trap_20
trap_20:
    pushq $0
    pushq $20
    jmp trap_common

.global trap_21
trap_21:
    pushq $21
    jmp trap_common

.global trap_22
trap_22:
    pushq $0
    pushq $22
    jmp trap_common

.global trap_23
trap_23:
    pushq $0
    pushq $23
    jmp trap_common

.global trap_24
trap_24:
    pushq $0
    pushq $24
    jmp trap_common

.global trap_25
trap_25:
    pushq $0
    pushq $25
    jmp trap_common

.global trap_26
trap_26:
    pushq $0
    pushq $26
    jmp trap_common

.global trap_27
trap_27:
    pushq $0
    pushq $27
    jmp trap_common

.global trap_28
trap_28:
    pushq $0
    pushq $28
    jmp trap_common

.global trap_29
trap_29:
    pushq $29
    jmp trap_common

.global trap_30
trap_30:
    pushq $30
    jmp trap_common

.global trap_31
trap_31:
    pushq $0
    pushq $31
    jmp trap_common

.global trap_32
trap_32:
    pushq $0
    pushq $32
    jmp trap_common

.global trap_128
trap_128:
    pushq $0
    pushq $128
    jmp trap_common

.section .rodata
.global exception_stubs
exception_stubs:
    .quad trap_0
    .quad trap_1
    .quad trap_2
    .quad trap_3
    .quad trap_4
    .quad trap_5
    .quad trap_6
    .quad trap_7
    .quad trap_8
    .quad trap_9
    .quad trap_10
    .quad trap_11
    .quad trap_12
    .quad trap_13
    .quad trap_14
    .quad trap_15
    .quad trap_16
    .quad trap_17
    .quad trap_18
    .quad trap_19
    .quad trap_20
    .quad trap_21
    .quad trap_22
    .quad trap_23
    .quad trap_24
    .quad trap_25
    .quad trap_26
    .quad trap_27
    .quad trap_28
    .quad trap_29
    .quad trap_30
    .quad trap_31
    .quad trap_32
.section .note.GNU-stack,"",@progbits
