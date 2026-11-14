/* GRUB loads this Multiboot2 kernel in 32-bit protected mode; the preflight
 * code below switches the CPU into long mode, then calls the 64-bit C entry.
 * The Multiboot2 header requests memory information and a 1280x800x32
 * framebuffer from GRUB's i386-pc build. */

.set MAGIC,    0xE85250D6
.set ARCH,     0               /* i386 protected mode; we switch ourselves. */
.set HEADER_LENGTH, header_end - header_start
.set CHECKSUM, -(MAGIC + ARCH + HEADER_LENGTH)

.section .multiboot,"a"
.align 8
header_start:
    .long MAGIC
    .long ARCH
    .long HEADER_LENGTH
    .long CHECKSUM

    /* Information request: memory info (4), memory map (6), framebuffer (8). */
    .word 1                       /* Type: information request. */
    .word 0                       /* Flags: required. */
    .long 20                      /* Tag size: header + three u32 requests. */
    .long 4, 6, 8                 /* memory info, memory map, framebuffer. */
    .balign 8                     /* Multiboot2 tags are walked 8-byte aligned. */

    /* Preferred graphics mode: 1280x800x32. */
    .word 5                       /* Type: framebuffer. */
    .word 0                       /* Flags: required. */
    .long 20                      /* Tag size. */
    .long 1280, 800, 32
    .balign 8                     /* Size 20 must pad to 24 before the end tag. */

    /* End of tags. */
    .word 0
    .word 0
    .long 8
header_end:

/* Page-table storage lives in BSS.  We identity-map the whole first 4 GB
 * with 2 MiB pages; every physical address the kernel can name (kernel,
 * Multiboot2 information, framebuffer) is below that boundary. */
.section .bss,"aw",@nobits
.align 4096
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd0:
    .skip 4096
pd1:
    .skip 4096
pd2:
    .skip 4096
pd3:
    .skip 4096
.comm boot_magic, 4, 4
.comm boot_info, 4, 4

/* C needs a stack.  The old 16 KiB stack is too small for the 64-bit ABI and
 * the DOOM bridge's call depth, so reserve 64 KiB. */
.align 16
stack_bottom:
    .skip 65536
stack_top:

.section .data
.align 8
gdt:
    .quad 0                       /* NULL descriptor. */
    .quad 0x00209A000000FFFF      /* 64-bit code: present, exec/read, L=1. */
gdt_end:
gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt

.section .text,"ax"
.code32
.global _start
.type _start, @function
.extern kernel_main
_start:
    cli
    cld
    /* Preserve GRUB's Multiboot2 magic and information pointer across the
     * mode switch.  Both are physical addresses below 4 GiB. */
    movl %eax, boot_magic
    movl %ebx, boot_info

    movl $stack_top, %esp
    xorl %ebp, %ebp

    /* Build the page tables (identity map): PML4 -> PDPT -> four PDs. */
    movl $pdpt, %eax
    orl $0x3, %eax                  /* PRESENT | WRITABLE */
    movl %eax, pml4
    movl $pd0, %eax
    orl $0x3, %eax
    movl %eax, pdpt
    movl $pd1, %eax
    orl $0x3, %eax
    movl %eax, pdpt + 8
    movl $pd2, %eax
    orl $0x3, %eax
    movl %eax, pdpt + 16
    movl $pd3, %eax
    orl $0x3, %eax
    movl %eax, pdpt + 24

    /* Fill 4 GiB / 2 MiB = 2048 large-page entries. */
    xorl %ecx, %ecx                 /* global page index */
fill_pages:
    movl %ecx, %eax
    shll $21, %eax                  /* physical address = index << 21 */
    orl $0x83, %eax                 /* PRESENT | WRITABLE | PS */
    cmp $512, %ecx
    jb .Lpd0
    cmp $1024, %ecx
    jb .Lpd1
    cmp $1536, %ecx
    jb .Lpd2
    movl %ecx, %edx
    sub $1536, %edx
    shll $3, %edx
    movl %eax, pd3(,%edx,1)
    jmp 1f
.Lpd2:
    movl %ecx, %edx
    sub $1024, %edx
    shll $3, %edx
    movl %eax, pd2(,%edx,1)
    jmp 1f
.Lpd1:
    movl %ecx, %edx
    sub $512, %edx
    shll $3, %edx
    movl %eax, pd1(,%edx,1)
    jmp 1f
.Lpd0:
    movl %ecx, %edx
    shll $3, %edx
    movl %eax, pd0(,%edx,1)
1:
    incl %ecx
    cmp $2048, %ecx
    jne fill_pages

    movl $pml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $((1 << 5) | (1 << 9) | (1 << 10)), %eax
    /* PAE | OSFXSR | OSXMMEXCPT: the x86_64 ABI returns doubles in SSE. */
    movl %eax, %cr4

    movl $0xC0000080, %ecx          /* EFER MSR */
    rdmsr
    orl $(1 << 8), %eax             /* Long Mode Enable */
    wrmsr

    movl %cr0, %eax
    orl $(1 << 31), %eax            /* Paging enable */
    movl %eax, %cr0

    lgdt gdt_ptr
    ljmp $0x08, $_start64

.code64
.global _start64
.type _start64, @function
_start64:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl boot_magic, %edi           /* Multiboot2 magic (zero-extended). */
    movl boot_info, %esi            /* Information pointer. */
    xorq %rbp, %rbp
    movq $stack_top, %rsp
    call kernel_main

    /* There is no parent program to return to. */
1:
    cli
    hlt
    jmp 1b
.size _start64, . - _start64

    .section .note.GNU-stack,"",@progbits
