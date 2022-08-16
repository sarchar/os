; This file started from: https://wiki.osdev.org/Bare_Bones_with_NASM

; even though we're producing an elf64 binary, this bootstrapping code is ran in 32-bit protected mode
bits 32

; Declare constants for the multiboot 2 header.
MB2_ALIGN    equ  1 << 0                    ; align loaded modules on page boundaries
MB2_MEMINFO  equ  1 << 1                    ; provide memory map
MB2_FLAGS    equ  MB2_ALIGN | MB2_MEMINFO   ; this is the Multiboot 'flag' field
MB2_ARCH     equ  0                         ; i386 protected mode
MB2_MAGIC    equ  0xE85250D6                ; 'magic number' lets bootloader find the header
MB2_HDRLEN   equ  multiboot_end - multiboot_header ; length of the header
MB2_CHECKSUM equ 0x100000000 - (MB2_MAGIC + MB2_ARCH + MB2_HDRLEN)  ; checksum
 
; Declare a multiboot header that marks the program as a kernel. These are magic
; values that are documented in the multiboot standard. The bootloader will
; search for this signature in the first 8 KiB of the kernel file, aligned at a
; 32-bit boundary. The signature is in its own section so the header can be
; forced to be within the first 8 KiB of the kernel file.
section .multiboot.data align=8
multiboot_header:
    ; magic fields
	dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_HDRLEN
	dd MB2_CHECKSUM
    ; tags
.console_tag:
;    dw 4 ; MULTIBOOT_HEADER_TAG_CONSOLE_FLAGS
;    dw 0 ; required
;    dd .framebuffer_tag - .console_tag
;    dw 3 ; 1<<0 = console required, 1<<1 = EGA supported
;    align 8
.framebuffer_tag:
    dw 5 ; MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw 1 ; MULTIBOOT_HEADER_TAG_OPTIONAL
    dd .efi_boot_services_tag - .framebuffer_tag
    dd 1024
    dd 768
    dd 32
    align 8
.efi_boot_services_tag:
;    dw 7 ; MULTIBOOT_HEADER_TAG_EFI_BS
;    dw 1 ; optional
;    dd .end_tag - .efi_boot_services_tag
;    align 8
.end_tag:
    dw 0                                 ; type (0 = terminating tag)
	dw 0                                 ; flags
    dd 8                                 ; size of tag
multiboot_end:

; Access bits
PRESENT        equ 1 << 7
DPL0           equ 0 << 5
DPL1           equ 1 << 5
DPL2           equ 2 << 5
DPL3           equ 3 << 5
NOT_SYS        equ 1 << 4
EXEC           equ 1 << 3
DC             equ 1 << 2
RW             equ 1 << 1
ACCESSED       equ 1 << 0

; Flags bits
GRAN_4K       equ 1 << 7
SZ_32         equ 1 << 6
LONG_MODE     equ 1 << 5

; Export the pointer to the GDT info so APs can use it
GDT:
.null: equ $ - GDT                              ; define the null segment
    dd (0x0000 << 16) | 0x0000                  ; Limit & Base
    db 0x00                                     ; Base
    db 0x00                                     ; Access
    db 0x00                                     ; Flags & Limit
    db 0x00                                     ; Base (high, bits 24-31)
.text: equ $ - GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | EXEC | RW            ; Access
    db GRAN_4K | LONG_MODE | 0x0F               ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.data: equ $ - GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | RW                   ; Access
    db GRAN_4K | SZ_32 | 0x0F                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.user_text:
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | EXEC | RW | DPL3     ; Access
    db GRAN_4K | LONG_MODE | 0x0F               ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.user_data:
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | RW | DPL3            ; Access
    db GRAN_4K | SZ_32 | 0x0F                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.tss: equ $ - GDT
    dd 0x00000068
    dd 0x00CF8900
.pointer:
    dw $ - GDT - 1                              ; Limit (size) of the GDT
    dq GDT                                      ; 64-bit base

; the page tables separated from .bss, since .bss will be placed in high mem
section .multiboot.bss align=8 nobits

; Reserve space for the page table
align 4096
global boot_page_table_level4:data
boot_page_table_level4: resq 512   ; one entry in this table is a physical address to a level 3 table (512 entries * 512GiB = 256TiB)
boot_page_table_level3: resq 512   ; one entry in this table is a physical address to a level 2 table (512 entries * 1GiB = 512GiB)
; another page table required for mapping 0xFFFFFFFE00000000-0xFFFFFFFEFFFFFFFF
boot_high_page_table_level3: resq 512   ; one entry in this table is a physical address to a level 2 table (512 entries * 1GiB = 512GiB)
                                        ; but will point to boot_page_table_XXXX's to also map real low memory into high virtual memory
;boot_page_table_level2: resq 512   ; one entry in this table is a physical address to a level 1 table (512 entries * 0x200000 = 1GiB)
;boot_page_table_level1: resq 512   ; one entry in this table is a 64-bit address to a 4,096 (0x1000) block of physical memory for a total 
;                                   ; of 512 * 0x1000 = 0x200000 = 2MiB mappable bytes per level 1 table

; these four are used as level 2 pages with huge page bit set, so with 1 entry mapping
; an entire level 1 page (2MiB), and 512 entries, we have 1GiB per table. To map the entire
; 32-bit space, we need four tables.
boot_page_table_0000: resq 512
boot_page_table_4000: resq 512
boot_page_table_8000: resq 512
boot_page_table_c000: resq 512

section .bss align=8 nobits
; The multiboot standard does not define the value of the stack pointer register
; (esp) and it is up to the kernel to provide a stack. This allocates room for a
; small stack by creating a symbol at the bottom of it, then allocating 16384
; bytes for it, and finally creating a symbol at the top. The stack grows
; downwards on x86. The stack is in its own section so it can be marked nobits,
; which means the kernel file is smaller because it does not contain an
; uninitialized stack. The stack on x86 must be 16-byte aligned according to the
; System V ABI standard and de-facto extensions. The compiler will assume the
; stack is properly aligned and failure to align the stack will result in
; undefined behavior.
align 16
global _stack_bottom:function (_stack_top-_stack_bottom)
global _stack_top:function (0)
_stack_bottom: resb 16*1024 ; 16 KiB
_stack_top:

; Link script defined symbols used for knowing the size of the kernel
extern _kernel_load_address
extern _kernel_end_address
extern _kernel_vma_base

; C entry point
extern kernel_main

section .data
align 16

; The linker script specifies _bootstrap_start as the entry point to the kernel and the
; bootloader will jump to this position once the kernel has been loaded. It
; doesn't make sense to return from this function as the bootloader is gone.
; Declare _bootstrap_start as a function symbol with the given symbol size.
section .multiboot.text
global _bootstrap_start:function (_bootstrap_start.end - _bootstrap_start)
_bootstrap_start:
	; The bootloader has loaded us into 32-bit protected mode on a x86
	; machine. Interrupts are disabled. Paging is disabled. The processor
	; state is as defined in the multiboot standard.  

    ; Before switching into long mode, set up paging. Start by pointing level 4 to level 3
    mov edi, boot_page_table_level4      
    mov eax, boot_page_table_level3
    or eax, 0x03  ; set present and writable flag
    mov dword [edi + 0], eax  ; set the 0th entry of the level 4 to point to our lavel 3 table (maps the first 512GiB of memory)

    ; The 511th entry in boot_page_table_level4 maps region 0xFFFF_FF80_0000_0000-0xFFFF_FFFF_FFFF_FFFF, so we'll point our high table there
    mov eax, boot_high_page_table_level3
    or eax, 0x03  ; set present and writable flag
    mov dword [edi + 511 * 8], eax

    ; Point the level 3 entries 0 through 3 to level 2 tables .. this is to map identity map the entire 4GiB address space
    ; and, the 504th entry in these tables corresponds to 0x7E_0000_0000, that's 1GiB (0x4000_0000) chunks, so map those into the high table
    mov edi, boot_page_table_level3      ; low page table
    mov edx, boot_high_page_table_level3 ; high page table
    mov eax, boot_page_table_0000
    or eax, 0x03  ; set present and writable flag
    mov dword [edi + 0 * 8], eax    ; set the 0th entry of the level 3 table to point to our level 2 table (maps the first 1GiB of memory)
    mov dword [edx + 504 * 8], eax  ; set the 504th entry of the level 3 table to point to our level 2 table (maps the first 1GiB of memory to 0x7E_0000_0000)

    ; then 4000
    mov eax, boot_page_table_4000
    or eax, 0x03  ; set present and writable flag
    mov dword [edi + 1 * 8], eax    ; set the 1th entry of the level 3 table to point to our level 2 table (maps the second 1GiB of memory)
    mov dword [edx + 505 * 8], eax  ; set the 505th entry of the level 3 table to point to our level 2 table (maps the second 1GiB of memory to 0x7E_4000_0000)

    ; then 8000
    mov eax, boot_page_table_8000
    or eax, 0x03                    ; set present and writable flag
    mov dword [edi + 2 * 8], eax    ; set the 2nd entry of the level 3 table to point to our level 2 table (maps the third 1GiB of memory)
    mov dword [edx + 506 * 8], eax  ; set the 506th entry of the level 3 table to point to our level 2 table (maps the third 1GiB of memory to 0x7E_8000_0000)

    ; and finally C000
    mov eax, boot_page_table_c000
    or eax, 0x03                    ; set present and writable
    mov dword [edi + 3 * 8], eax    ; set the 3rd entry of the level 3 table to point to our level 2 table (maps the fourth 1GiB of memory)
    mov dword [edx + 507 * 8], eax  ; set the 507th entry of the level 3 table to point to our level 2 table (maps the fourth 1GiB of memory to 0x7E_C000_0000)

    ; Each of the tables (0000, 4000, 8000, C000) map 1GiB using huge pages, so all four need to be set up
    mov ecx, 512*4     ; number of entries per table is 512, but all four tables are consecutive in memory so we can fill in one loop
    xor eax, eax       ; start with the very last 2MiB
    or eax, 0b10000011 ; or in present, writable, huge pages bits

    ; map the 4GiB
    mov edi, boot_page_table_0000 ; put pagetable for first 1GiB into edi
.fill_0000:
    sub eax, 0x200000  ; decrement by 2MiB
    mov dword [edi + (ecx - 1) * 8], eax
    loop .fill_0000

    ; Set control register 3 to the address of the level 4 page table
    mov ecx, boot_page_table_level4
    mov cr3, ecx

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    
    ; Enable long mode
    mov ecx, 0xC0000080     ; select register
    rdmsr                   ; read value
    or eax, 1 << 8          ; set LM bit
    wrmsr                   ; write value

    ; Finally, enable paging
    mov ecx, cr0
    or ecx, 1 << 31
    or ecx, 1 << 16
    mov cr0, ecx

    ; at this point we're still in a 32-bit compatibility submode, and we switch to long mode
    ; by setting the long mode flag on the code segment in the gdt and jumping to code in it
    lgdt [GDT.pointer]

    ; we have to load the segment registers with the selectors into the gdt
    mov ax, GDT.data       ; set the non-code segment registers to the data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; finally, this jump moves us into long mode
    ; TODO this jumps to a 32-bit label (relocation size errors when using _start), so at some point
    ; it would be nice to find out how to do a jump to a 64-bit address. Or maybe this is the right
    ; way to do it?
    jmp GDT.text:.long_mode_entry  ; loads CS register, where the GDT entry has long mode set

.long_mode_entry:  ; label remains in 32-bit land
    ; and falls into 64-bit code
bits 64
    mov rdi, _start ; indirect jump to 64-bit address
    jmp rdi
.end:

; We have a _start function running in high (virtual) memory now
section .text
bits 64

global _start:function (_start.end - _start)
_start:
	; To set up a stack, we set the esp register to point to the top of our
	; stack (as it grows downwards on x86 systems). This is necessarily done
	; in assembly as languages such as C cannot function without a stack.
	mov rsp, _stack_top

    ; ebx was preserved in _bootstrap_start, put it into rdi for the first parameter to kernel_main
    mov rdi, rbx

	; Enter the high-level kernel. The ABI requires the stack be 16-byte
	; aligned at the time of the call instruction (which afterwards pushes
	; the return pointer of size 4 bytes). The stack was originally 16-byte
	; aligned above and we've since pushed a multiple of 16 bytes to the
	; stack since (pushed 0 bytes so far) and the alignment is thus
	; preserved and the call is well defined.
	call kernel_main

	; If the system has nothing more to do, put the computer into an
	; infinite loop. To do that:
	; 1) Disable interrupts with cli (clear interrupt enable in eflags).
	;    They are already disabled by the bootloader, so this is not needed.
	;    Mind that you might later enable interrupts and return from
	;    kernel_main (which is sort of nonsensical to do).
	; 2) Wait for the next interrupt to arrive with hlt (halt instruction).
	;    Since they are disabled, this will lock up the computer.
	; 3) Jump to the hlt instruction if it ever wakes up due to a
	;    non-maskable interrupt occurring or due to system management mode.
	cli
.hang:	
	hlt
	jmp .hang
.end:

; relocate the GDT into high kernel memory
; rdi has new offset to add to the gdt values
global _gdt_fixup:function (_gdt_fixup.end - _gdt_fixup)
_gdt_fixup:
    mov rsi, [rdi+GDT.pointer+2]    ; read the 64-bit address at GDT.pointer+2 (base to GDT), but add _kernel_vma_base
                                    ; so that it is a virtual address already mapped
    add rsi, rdi                    ; add the virtual address to the GDT base
    mov [rdi+GDT.pointer+2], rsi    ; store it in GDT.pointer+2
    add rdi, GDT.pointer            ; load GDT.pointer+_kernel_vma_base into rdi
    lgdt [rdi]                      ; reload the GDT
    ret
.end:
