bits 64

section .text align=8
extern _interrupt_kb_handler
extern _lapic_eoi
extern kernel_panic

global _interrupt_handler_common:function (_interrupt_handler_common.end - _interrupt_handler_common)
_interrupt_handler_common:
    ; the caller to this function has placed the error code, if any, into rdi, the actual irq handler into rax
    ; and has saved both of those registers before jumping here
    push rdx            ; Save registers. TODO save allll the registers.

    cld                 ; C code following the SysV ABI requires DF to be clear on function entry
    call rax            ; Call the C function handler
    call _lapic_eoi     ; end of interrupt

    pop rdx             ; Restore all the registers
    pop rsi
    pop rdi
    pop rax

    iretq               ; Return from interrupt
.end:
