/* Based on code from https://wiki.osdev.org/Bare_Bones */

/* The bootloader will look at this image and start execution at the symbol designated as the entry point. */
ENTRY(_bootstrap_start)
 
/* Tell where the various sections of the object files will be put in the final kernel image. */
SECTIONS
{
    /* Begin putting sections at physical address 0x100000 (1 MiB), a conventional address for kernels to be loaded by the bootloader. */
    . = 1M;
 
    /* Define the kernel start address symbol */
    _kernel_start_address = .;

    /* The multiboot code/kernel entry point runs with paging disable and will run at address 1MiB */
    .multiboot.data : {
        *(.multiboot.data)
    }

    .multiboot.text : ALIGN(4K) {
        *(.multiboot.text)
    }

    /* Actual kernel code will exist at virtual address 0xC0000000+1MiB */
    . += 0xC0000000;

    /* Build the text (code) section */
    .text ALIGN(4K) : AT (ADDR(.text) - 0xC0000000) {
        *(.text)
    }

    /* Read-only data. */
    .rodata ALIGN(4K) : AT (ADDR(.rodata) - 0xC0000000) {
        *(.rodata)
    }
 

    /* Read-write data (uninitialized) and stack */
    .bss ALIGN(4K) : AT (ADDR(.bss) - 0xC0000000) {
        *(COMMON)
        *(.bss)
        *(.bootstrap_stack)
    }

    /* Read-write data (initialized) */
    .data ALIGN(4K) : AT (ADDR(.data) - 0xC0000000) {
        *(.data)
    }
 
    /* Define a symbol for the end of the kernel */
    _kernel_end_address = .;
}
