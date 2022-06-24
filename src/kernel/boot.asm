; This file started from: https://wiki.osdev.org/Bare_Bones_with_NASM

; Declare constants for the multiboot header.
MBALIGN  equ  1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ  1 << 1            ; provide memory map
FLAGS    equ  MBALIGN | MEMINFO ; this is the Multiboot 'flag' field
MAGIC    equ  0x1BADB002        ; 'magic number' lets bootloader find the header
CHECKSUM equ -(MAGIC + FLAGS)   ; checksum of above, to prove we are multiboot
 
; Declare a multiboot header that marks the program as a kernel. These are magic
; values that are documented in the multiboot standard. The bootloader will
; search for this signature in the first 8 KiB of the kernel file, aligned at a
; 32-bit boundary. The signature is in its own section so the header can be
; forced to be within the first 8 KiB of the kernel file.
section .multiboot.data
align 4
multiboot_header:
	dd MAGIC
	dd FLAGS
	dd CHECKSUM

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
section .bootstrap_stack nobits align=16
stack_bottom: resb 16*1024 ; 16 KiB
stack_top:

; Reserve space for the page table
section .bss
align 4096
boot_page_directory: resb 4096 ; one page
boot_page_table1:    resb 4096 ; one page

extern _kernel_start_address
extern _kernel_end_address

; Define the default GDT
section .data
align 16

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

GDT:
.null: equ $ - GDT                              ; define the null segment
    dd (0x0000 << 16) | 0x0000                  ; Limit & Base
    db 0x00                                     ; Base
    db 0x00                                     ; Access
    db 0x00                                     ; Flags & Limit
    db 0x00                                     ; Base (high, bits 24-31)
.code: equ $ - GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | DPL0 | NOT_SYS | EXEC | RW     ; Access
    db GRAN_4K | SZ_32 | 0xFF                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.data: equ $ - GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | RW                   ; Access
    db GRAN_4K | SZ_32 | 0xFF                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.tss: equ $ - GDT
    dd 0x00000068
    dd 0x00CF8900
.pointer:
    dw $ - GDT - 1                              ; Limit (size) of the GDT
    dd GDT                                      ; 64-bit base, even if only 32-bits are used
    dd 0

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

    ; boot_page_table1 is in .bss, so it is a virtual address. subtract the virtual base to get the physical address
    mov edi, (boot_page_table1 - 0xC0000000)

    ; create virtual address 0x00000000
    mov esi, 0x00000000
    mov ecx, 1023 ; map 4MiB (minus 4096 bytes) (loop 1023 times)

.boot_pt_loop:
    cmp esi, _kernel_start_address              ; is the current address in ESI below current space?
    jl _bootstrap_start.next_boot_pt            ; if so, don't map it
    cmp esi, (_kernel_end_address - 0xC0000000) ; did we map all of the kernel space?
    jge _bootstrap_start.done_boot_pt           ; if so, we're done

    ; current esi address is within kernel space, so map it
    mov edx, esi       ; address into edx
    or edx, 0x03       ; set the lower two bits to 1 (present, writable)
    mov [edi], edx     ; write the address into the page table (edi = physical address of boot_page_table1)

.next_boot_pt:
    add esi, 4096      ; increment address by 1 page
    add edi, 4         ; increment page table by one entry (4 bytes)
    loop .boot_pt_loop ; next page

.done_boot_pt:
    ; map VGA ram (0xB8000) to the last entry in the page table. the last entry is 1023, so that 
    ; offset is 1023*4096 = 0x3FF000
    mov edi, (boot_page_table1 - 0xC0000000) ; reload the physical address of the page table
    add edi, 1023 * 4                        ; add an offset into the table for the last entry
    mov dword [edi], (0x000B8000 | 0x03)     ; set the page table with present, writable set

    ; The page table built maps whatever virtual address (selected by the entry in the page table directory)
    ; to physical addresses 0x3FFFFF. Identity map virtual address 0 to 0 so that our code continues to execute
    ; normally, and map the same page table to 0xC0000000
    mov edi, (boot_page_directory - 0xC0000000)   ; pointer to page directory
    mov ecx, (boot_page_table1 - 0xC0000000)      ; physical address of the page table
    or ecx, 0x03                                  ; set control bits on the address
    mov dword [edi], ecx                          ; write pointer to the page table into entry 0 of the directory
    add edi, 768 * 4                              ; offset into the directory for virtual address 0xC0000000
    mov dword [edi], ecx                          ; write the same page table address

    ; Set control register 3 to the address of the page directory
    mov ecx, (boot_page_directory - 0xC0000000)
    mov cr3, ecx

    ; Finally, enable paging
    mov ecx, cr0
    or ecx, 0x80010000
    mov cr0, ecx

    ; Jump to high virtual address
    lea ecx, [_start] ; will put a virtual address for _start into ecx
    jmp ecx           ; absolute jump

.end:

; We have a _start function running in high (virtual) memory now
section .text
global _start:function (_start.end - _start)
_start:
	; To set up a stack, we set the esp register to point to the top of our
	; stack (as it grows downwards on x86 systems). This is necessarily done
	; in assembly as languages such as C cannot function without a stack.
	mov esp, stack_top

    ; Unmap the identity mapping in directory entry 0
    mov dword [boot_page_directory + 0], 0

    ; Reload cr3 to force a TLB flush so the changes take effect immediately
    mov ecx, cr3
    mov cr3, ecx

	; This is a good place to initialize crucial processor state before the
	; high-level kernel is entered. It's best to minimize the early
	; environment where crucial features are offline. Note that the
	; processor is not fully initialized yet: Features such as floating
	; point instructions and instruction set extensions are not initialized
	; yet. The GDT should be loaded here. Paging should be enabled here.
	; C++ features such as global constructors and exceptions will require
	; runtime support to work as well.

    ;!; enable long mode by setting the LM bit in the EFER MSR
    ;!mov ecx, 0xC0000080 ; select the EFER MSR
    ;!rdmsr               ; read, result in eax
    ;!or eax, 1 << 8      ; set bit 8 (LM)
    ;!wrmsr               ; write

    ;!; enable paging, which is required for long mode
    ;!mov eax, cr0        ; get control register 0
    ;!or eax, 1 << 31     ; set the PG bit
    ;!mov cr0, eax        ; write control register 0

    ;!; at this point we're still in a 32-bit compatibility submode, and we switch to long mode
    ;!; by setting the long mode flag on the code segment in the gdt and jumping to code in it
;!    lgdt [GDT.pointer]
;! 
;!    ; we have to load the segment registers with the selectors into the gdt
;!    jmp GDT.code:.reload_segment_registers  ; start by loading CS
;!.reload_segment_registers:
;!    mov eax, GDT.data                       ; set the remaining segment registers to the data segment
;!    mov ds, ax
;!    mov es, ax
;!    mov fs, ax
;!    mov gs, ax
;!    mov ss, ax

	; Enter the high-level kernel. The ABI requires the stack is 16-byte
	; aligned at the time of the call instruction (which afterwards pushes
	; the return pointer of size 4 bytes). The stack was originally 16-byte
	; aligned above and we've since pushed a multiple of 16 bytes to the
	; stack since (pushed 0 bytes so far) and the alignment is thus
	; preserved and the call is well defined.
	; note, that if you are building on Windows, C functions may have "_" prefix in assembly: _kernel_main
	extern kernel_main
	;!call GDT.code:kernel_main
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

