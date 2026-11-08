ENTRY(_start)

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(8) :
    {
        KEEP(*(.multiboot))
        KEEP(*(.multiboot2))
    }

    .text ALIGN(4K) :
    {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data .data.*)
    }

    /* End of what is actually loaded from the ELF file. The 2 MiB alignment
       of .userbss below leaves a hole after this point that holds nothing;
       the RAM accounting in kernel.c skips it instead of calling it "used". */
    _loaded_end = .;

    /* Ring-3 region: its own 2 MiB page, made user-accessible by arch_init(). */
    .userbss ALIGN(2M) (NOLOAD) :
    {
        _userbss_start = .;
        *(.bss.userregion)
        _userbss_end = .;
    }

    .bss ALIGN(4K) (NOLOAD) :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
    }

    /* End of the kernel image incl. .bss: framebuffer.c keeps its back
       buffer out of everything below this. */
    PROVIDE(_end = .);

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
