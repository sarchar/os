bits 64

section .text align=8
extern _interrupt_kb_handler
extern _send_lapic_eoi
extern kernel_panic

global _interrupt_handler_common:function (_interrupt_handler_common.end - _interrupt_handler_common)
_interrupt_handler_common:
    ; the caller to this function has placed the error code, if any, into rdi, the actual irq handler into rax
    ; and has saved both of those registers before jumping here

    ; check if GS needs to be swapped by checking if the code segment is selected to be the second entry in the GDT (= offset 8)
    ; with 4 registers pushed onto stack the before _interrupt_handler_common, there will be a pc and cs before that
    ; TODO this won't be useful until we have ring 3 code
    ; see https://wiki.osdev.org/SWAPGS
    cmp word [rsp+5*8], 0x08
    je .s1
    swapgs
.s1:

    ; TODO save allll the registers.

    cld                  ; C code following the SysV ABI requires DF to be clear on function entry
    call rax             ; Call the C function handler
    call _send_lapic_eoi ; end of interrupt

    ; TODO Restore alllll the registers

    ; these registers were saved before the call to _interrupt_handler_common
    pop rsi
    pop rdi
    pop rdx
    pop rax

    ; check cs here
    cmp word [rsp+1*8], 0x08 ; only rip on the stack now
    je .s2
    swapgs
.s2:

    iretq               ; Return from interrupt
.end:
