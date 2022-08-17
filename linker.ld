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

    /* Place the ap trampoline actual code at physical address in kernel vma space */
    . = ALIGN(8);        /* align to 64 bit */
    _ap_boot_start = .;  /* define global symbol where the ap boot page is at */
    .ap_boot 0x8000 : AT(_ap_boot_start - _kernel_vma_base) { /* set vma=0x8000 and load address to be immediately after .bss */
        *(.ap_boot.text) /* text must be first since PC starts at 0x8000 */
        . = ALIGN(8);    /* alignment for generic data */
        *(.ap_boot.data) /* .data after .text */
    } 
    _ap_boot_size = SIZEOF(.ap_boot); /* define the length of the ap_boot data */

    /* Define a symbol for the end of the kernel */
    . = _ap_boot_start + _ap_boot_size;
    . = ALIGN(4K);           /* align up to page size */
    _kernel_end_address = .;

    /* TEMP include some userland code. it's placed outside of "kernel" memory so that we can memory map it with user permissions */

    /* Build the text (code) section */
    _userland_text_start = .;
    .userland.text ALIGN(4K) : AT (ADDR(.userland.text) - _kernel_vma_base) {
        *(.userland.text)
    }
    _userland_text_end = .;

    /* Read-only data. */
    . = ALIGN(4K);           /* align up to page size */
    _userland_data_start = .;
    .userland.rodata ALIGN(4K) : AT (ADDR(.userland.rodata) - _kernel_vma_base) {
        *(.userland.rodata)
    }

    /* Read-write data (initialized) */
    .userland.data ALIGN(4K) : AT (ADDR(.userland.data) - _kernel_vma_base) {
        *(.userland.data)
    }
 
    /* Read-write data (uninitialized) and stack */
    .userland.bss ALIGN(4K) : AT (ADDR(.userland.bss) - _kernel_vma_base) {
        *(.userland.bss)
    }
    _userland_data_end = .;

    /DISCARD/ : {
        *(.comment)
    }
}
