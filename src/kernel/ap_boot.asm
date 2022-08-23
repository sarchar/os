extern boot_page_table_level4
extern ap_main

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

section .ap_boot.text
bits 16

_ap_boot:
    cli
    cld

    ; clear cs to be 0, the null segment
    jmp 0x00:.clear_cs
align 16
.clear_cs:

    ; setup ds to make sure it's 0 before loading gdt
    xor ax, ax
    mov ds, ax
    mov fs, ax  ; fs and gs get 0 for their descriptors
    mov gs, ax
    lgdt [ap_boot_GDT.pointer]

    ; set all the data segments to the 2nd entry in the gdt
    mov ax, ap_boot_GDT.data
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; set protected mode bit in cr0
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; jump to complete the transition to protected mode with proper cs
    jmp ap_boot_GDT.text:.protected_mode

bits 32
align 16
.protected_mode:
    ; now switch into long mode
    ; first, use the initial boot page table, which maps low memory
    mov eax, boot_page_table_level4
    mov cr3, eax

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

    xor eax, eax
    mov ds, ax
    lgdt [ap_boot_long_GDT.pointer]
    jmp ap_boot_long_GDT.text:.long_mode  ; loads CS register, where the GDT entry has long mode set

bits 64
.long_mode:
    ; in long mode, load the kernel page table. otherwise, our stack won't be accessible
    mov rsi, _ap_page_table
    mov rax, qword [rsi]
    mov cr3, rax

    ; set up the stack pointer. the boostrap processor placed our stack (top) in _ap_boot_stack_top
    mov rax, _ap_boot_stack_top
    mov rsp, [rax]

    ; determine local cpu apic id. this could also be a parameter passed in by
    ; the BSP, but we can also use cpuid here and they should match
    mov eax, 1
    cpuid
    shr ebx, 24
    mov rdi, rbx   ; first parameter to ap_main is of type u8, so this is fine
    
    ; jump into C code
    mov rsi, ap_main
    jmp rsi

.forever:
    jmp .forever

_ap_boot_end:

; GDT declared in ap_boot so that it's accessible in 16-bit mode
section .ap_boot.data align=64

align 64
ap_boot_GDT:
.null: equ $ - ap_boot_GDT                      ; define the null segment
    dq 0
.text: equ $ - ap_boot_GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | EXEC | RW            ; Access
    db GRAN_4K | SZ_32 | 0x0F                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.data: equ $ - ap_boot_GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | RW                   ; Access
    db GRAN_4K | SZ_32 | 0x0F                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.tss: equ $ - ap_boot_GDT
    dd 0x00000068
    dd 0x00CF8900
.pointer:
    dw ap_boot_GDT.pointer - ap_boot_GDT - 1
    dq ap_boot_GDT

; Set to the stack pointer to use for this AP
global _ap_boot_stack_top:data
_ap_boot_stack_top:
    dq 0

global _ap_page_table:data
_ap_page_table:
    dq 0

section .multiboot.data 
align 64
ap_boot_long_GDT:
.null: equ $ - ap_boot_long_GDT                 ; define the null segment
    dq 0
.text: equ $ - ap_boot_long_GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | EXEC | RW | DPL0     ; Access
    db GRAN_4K | LONG_MODE | 0x0F               ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.data: equ $ - ap_boot_long_GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | RW | DPL0            ; Access
    db GRAN_4K | SZ_32 | 0x0F                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.tss: equ $ - ap_boot_long_GDT
    ; 64-bit tss takes up two GDT entries
    dd 0x00000068                               ; Limit & Base
    db 0x00                                     ; Base (mid, bits 16-23)
    db 0x89                                     ; Access, bit 7 = present, bit 0..3 = TSS
    db 0xCF                                     ; Flags (0xC, 4KiB, SZ_32), Limit (high bits 16-19)
    db 0x00                                     ; Base (high bits 24-31)
    dd 0x00000000                               ; Base (long bits 32-63)
    dd 0x00000000                               ; Reserved
.pointer:
    dw ap_boot_long_GDT.pointer - ap_boot_long_GDT - 1
    dq ap_boot_long_GDT


