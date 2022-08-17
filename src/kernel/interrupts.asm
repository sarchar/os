bits 64

section .text align=8
extern _interrupt_kb_handler
extern _send_lapic_eoi
extern kernel_panic

global _interrupt_handler_common:function (_interrupt_handler_common.end - _interrupt_handler_common)
_interrupt_handler_common:
    ; at entry to this function, we have
    ;   - irq vector in rdi (arg0)
    ;   - fault/trap address in rsi (arg1)
    ;   - optionally, the error code in rcx (arg3)
    ;   - the C code function handle this interrupt in rax
    ;   - and 5 registers have been saved already: rax, rcx, rdx, rdi, rsi

    ; first, check if GSBase needs to be swapped by checking if the code segment is selected to be the second entry in the GDT (= offset 8)
    ; with 5 registers pushed onto stack the before _interrupt_handler_common, there will be a pc and cs before that
    ; see https://wiki.osdev.org/SWAPGS
    ;   rsp+6*8: cs
    ;   rsp+5*8: rip
    ;   rsp+4*8: rax
    ;   rsp+3*8: rcx
    ;   rsp+2*8: rdx
    ;   rsp+1*8: rdi
    ;   rsp+0*8: rsi
    cmp word [rsp+6*8], 0x08
    je .s1
    swapgs
.s1:
    ; the following registers are saved by the ABI in C code, so we can be sure they won't be
    ; modified. but we may need to save them for context switches
    push rbx
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; everything is pushed onto the stack, so rdx (arg2) needs a pointer to that structure
    mov rdx, rsp

    cld                  ; C code following the SysV ABI requires DF to be clear on function entry
    call rax             ; Call the C function handler
    call _send_lapic_eoi ; end of interrupt

    ; restore all the saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rbx

    ; these registers were saved before the call to _interrupt_handler_common
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rax

    ; check cs here
    cmp word [rsp+1*8], 0x08 ; only rip on the stack now
    je .s2
    swapgs
.s2:

    iretq               ; Return from interrupt
.end:
