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
    ; first set the boot page tables that identity map 0-4GB so we can continue booting
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

    xor eax, eax
    mov ds, ax
    lgdt [ap_boot_long_GDT.pointer]
    jmp ap_boot_long_GDT.text:.long_mode  ; loads CS register, where the GDT entry has long mode set

bits 64
.long_mode:
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

; relocate the GDT into high kernel memory
; rdi has new offset to add to the gdt values
; this is called once on the BSP
global _ap_gdt_fixup:function (_ap_gdt_fixup.end - _ap_gdt_fixup)
_ap_gdt_fixup:
    mov rsi, [rdi+ap_boot_long_GDT.pointer+2]    ; read the 64-bit address at ap_boot_long_GDT.pointer+2 (base to the GDT), but add _kernel_vma_base
                                                 ; so that it is a virtual address already mapped
    add rsi, rdi                                 ; add the virtual address to the GDT base
    mov [rdi+ap_boot_long_GDT.pointer+2], rsi    ; store it in ap_boot_long_GDT.pointer+2
    ret
.end:

; this is called on every AP ocne the table has been fixed up
global _ap_reload_gdt:function (_ap_reload_gdt.end - _ap_reload_gdt)
_ap_reload_gdt:
    add rdi, ap_boot_long_GDT.pointer            ; load ap_boot_long_GDT.pointer+_kernel_vma_base into rdi
    lgdt [rdi]                                   ; reload the GDT
    ret
.end:

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

section .multiboot.data 
align 64
ap_boot_long_GDT:
.null: equ $ - ap_boot_long_GDT                 ; define the null segment
    dq 0
.text: equ $ - ap_boot_long_GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | EXEC | RW            ; Access
    db GRAN_4K | LONG_MODE | 0x0F               ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.data: equ $ - ap_boot_long_GDT
    dd (0x0000 << 16) | 0xFFFF                  ; Limit & Base (low, bits 0-15)
    db 0x00                                     ; Base (mid, bits 16-23)
    db PRESENT | NOT_SYS | RW                   ; Access
    db GRAN_4K | SZ_32 | 0x0F                   ; Flags & Limit (high, bits 16-19)
    db 0x00                                     ; Base (high, bits 24-31)
.tss: equ $ - ap_boot_long_GDT
    dd 0x00000068
    dd 0x00CF8900
.pointer:
    dw ap_boot_long_GDT.pointer - ap_boot_long_GDT - 1
    dq ap_boot_long_GDT


