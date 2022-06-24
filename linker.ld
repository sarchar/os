/* Based on code from https://wiki.osdev.org/Bare_Bones */

/* The bootloader will look at this image and start execution at the symbol designated as the entry point. */
ENTRY(_start)
 
/* Tell where the various sections of the object files will be put in the final kernel image. */
SECTIONS
{
    /* Begin putting sections at address 0x100000 (1 MiB), a conventional address for kernels to be loaded by the bootloader. */
    . = 1M;
 
    /* Build the text (code) section */
    .text BLOCK(4K) : ALIGN(4K)
    {
        /* First put the multiboot header, as it is required to be put very early early in the image or the bootloader 
           won't recognize the file format. */
        *(.multiboot)

        /* Then actual .text sections */
        *(.text)
    }
 
    /* Read-only data. */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }
 
    /* Read-write data (initialized) */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }
 
    /* Read-write data (uninitialized) and stack */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
 
    /* The compiler may produce other sections, by default it will put them in a segment with the same name. */
    /* Simply add stuff here as needed. */
}
