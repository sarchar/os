; must match struct task in task.h
TASK_RIP_OFFSET               equ 0
TASK_RSP_OFFSET               equ 8
TASK_RFLAGS_OFFSET            equ 16
TASK_LAST_GLOBAL_TICKS_OFFSET equ 24
TASK_RUNTIME_OFFSET           equ 32
TASK_FLAGS_OFFSET             equ 40
TASK_ENTRY_OFFSET             equ 48

; must match enum TEST_FLAGS in task.h
TASK_FLAG_USER                equ (1 << 0)

; used for task timing
extern global_ticks

section .text
align 8
bits 64

; _task_switch_to_user(struct task* from_task (rdi), struct task* to_task (rsi))
global _task_switch_to:function (_task_switch_to.end - _task_switch_to)
_task_switch_to:
    ; rip is on the stack, save it in the task structure
    pop rdx
    mov [rdi+TASK_RIP_OFFSET], rdx

    ; for SysV ABI, functions must preserve rbx, rsp, rbp, r12, r13, r14, and r15 
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx

    ; save rflags
    pushfq
    pop rax
    mov [rdi+TASK_RFLAGS_OFFSET], rax

    ; save old stack pointer
    mov [rdi+TASK_RSP_OFFSET], rsp

    ; compute how much time has passed and add it to the runtime
    mov rbp, global_ticks
    mov rax, [rbp]
    sub rax, [rdi+TASK_LAST_GLOBAL_TICKS_OFFSET]
    add [rdi+TASK_RUNTIME_OFFSET], rax
    
    ; load new stack pointer
    mov rsp, [rsi+TASK_RSP_OFFSET]

    ;TODO set pagetable?

    ; set the global ticks value now that the process is running again
    mov rax, [rbp]
    mov [rsi+TASK_LAST_GLOBAL_TICKS_OFFSET], rax

.jump:
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15

    ; check if we're jumping to user code or not
;    test qword [rsi+TASK_FLAGS_OFFSET], TASK_FLAG_USER
;    jne .user

    ; for kernel tasks, just jump to task code
    jmp [rsi+TASK_RIP_OFFSET]

.end:

global _task_entry_userland:function (_task_entry_userland.end - _task_entry_userland)
_task_entry_userland:
    ; for user tasks, we push the stack segment, stack pointer, code segment, rflags, and finally the instruction pointer and use iretq
    ; iretq only pops rsp and ss if the code segment has a less privileged DPL than the current segment
    ; the current stack pointer (before we push the iretq requirements) is what the usercode will use, save it
    mov rdx, rsp

    ; set the data segments
    xor rax, rax
    mov ax, (4*8) | 3 ; user data GDT segment is at offset 0x20, OR'd with the new privilege level 3
    swapgs            ; save kernel gs
    mov gs, ax

    ; first push the stack segment (==user data segment)
    push rax

    ; push the stack pointer
    push rdx

    ; push rflags
    mov rax, [rsi+TASK_RFLAGS_OFFSET]
    or rax, 1 << 9 ; set IF flag
    push rax

    ; push the code segment selector
    xor rax, rax
    mov ax, 0x18 | 3 ; ap_boot_long_GDT.user_code is at offset 0x18, OR'd with the new privilege level 3
    push rax

    ; finally the entry point (not RIP), since this is the entry function, not the task switch
    mov rax, [rsi+TASK_ENTRY_OFFSET]
    push rax

    ; go to userland
    iretq
.end:

