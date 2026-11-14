ENTRY(_start)

PHDRS
{
    code PT_LOAD FLAGS(5); /* Read + execute. */
    data PT_LOAD FLAGS(6); /* Read + write. */
}

SECTIONS
{
    /* Load above the first MiB, which the firmware uses.  The kernel is
     * identity-mapped at the same address in long mode (see src/boot.S), so
     * physical and virtual addresses stay below 4 GiB. */
    . = 1M;
    _kernel_physical_start = .;

    /* GRUB must find this header in the first 32 KiB of the ELF file. */
    .multiboot : { KEEP(*(.multiboot)) } :code
    .text ALIGN(16) : { *(.text*) } :code
    .rodata ALIGN(4K) : { *(.rodata*) } :code
    .data ALIGN(4K) : { *(.data*) } :data
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) } :data
    _kernel_physical_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
