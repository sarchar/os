/* Based on code from https://wiki.osdev.org/Bare_Bones */

/* The bootloader will look at this image and start execution at the symbol designated as the entry point. */
ENTRY(_bootstrap_start)
 
/* Tell where the various sections of the object files will be put in the final kernel image. */
SECTIONS
{
    _kernel_vma_base = 0xFFFFFFFE00000000;

    /* Begin putting sections at physical address 0x100000 (1 MiB), a conventional address for kernels to be loaded by the bootloader. */
    . = 1M;
 
    /* Define the kernel start address symbol */
    _kernel_load_address = .;

    /* The multiboot code/kernel entry point runs with paging disable and will run at address 1MiB */
    .multiboot.data : {
        *(.multiboot.data)
    }

    .multiboot.bss : {
        *(.multiboot.bss)
    }

    .multiboot.text : ALIGN(4K) {
        *(.multiboot.text)
    }

    /* Actual kernel code will exist at virtual address _kernel_vma_base+1M */
    . += _kernel_vma_base;

    /* Build the text (code) section */
    .text ALIGN(4K) : AT (ADDR(.text) - _kernel_vma_base) {
        *(.text)
    }

    /* Read-only data. */
    .rodata ALIGN(4K) : AT (ADDR(.rodata) - _kernel_vma_base) {
        *(.rodata)
    }

    /* Read-write data (initialized) */
    .data ALIGN(4K) : AT (ADDR(.data) - _kernel_vma_base) {
        *(.data)
    }
 
    /* Read-write data (uninitialized) and stack */
    .bss ALIGN(4K) : AT (ADDR(.bss) - _kernel_vma_base) {
        *(COMMON)
        *(.bss)
    }

    /* Define a symbol for the end of the kernel */
    . = ALIGN(4K);           /* align up to page size */
    _kernel_end_address = .;

    /DISCARD/ : {
        *(.comment)
    }
}
