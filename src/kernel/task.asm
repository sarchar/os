TASK_RSP_OFFSET               equ 0
TASK_LAST_GLOBAL_TICKS_OFFSET equ 8
TASK_RUNTIME_OFFSET           equ 16

extern global_ticks

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

    ; compute how much time has passed and add it to the runtime
    mov rbp, global_ticks
    mov rax, [rbp]
    sub rax, [edi+TASK_LAST_GLOBAL_TICKS_OFFSET]
    add [edi+TASK_RUNTIME_OFFSET], rax
    
    ; load new stack pointer
    mov rsp, [rsi+TASK_RSP_OFFSET]

    ;TODO set the proper stack in TSS for this cpu
    ;TODO set pagetable?

    ; set the global ticks value for when the process started running again
    mov rax, [rbp]
    mov [rsi+TASK_LAST_GLOBAL_TICKS_OFFSET], rax

.done:
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15
    ret

.end:
