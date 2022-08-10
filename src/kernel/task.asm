TASK_RSP_OFFSET equ 0

section .text
align 8
bits 64

global _task_switch_to:function (_task_switch_to.end - _task_switch_to)
_task_switch_to:
    ; struct task* from_task (rdi), struct task* to_task (rsi)
    ; for SysV ABI, functions must preserve rbx, rsp, rbp, r12, r13, r14, and r15 
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

    ; save old stack pointer
    mov [edi+TASK_RSP_OFFSET], rsp

    ; load new stack pointer
    mov rsp, [rsi+TASK_RSP_OFFSET]
    ;TODO set the proper stack in TSS for this cpu
    ;TODO set pagetable?

.done:
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15
    ret

.end:
