bits 64

section .text align=8
extern _interrupt_kb_handler
extern _send_lapic_eoi
extern kernel_panic

global _interrupt_handler_common:function (_interrupt_handler_common.end - _interrupt_handler_common)
_interrupt_handler_common:
    ; the caller to this function has placed the error code, if any, into rdi, the actual irq handler into rax
    ; and has saved both of those registers before jumping here

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

    iretq               ; Return from interrupt
.end:
