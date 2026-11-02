; Exception / syscall stubs and the ring 0 <-> ring 3 transitions.
BITS 64

section .text

extern trap_dispatch

%macro ISR_NOERR 1
isr%1:
    push qword 0
    push qword %1
    jmp trap_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push qword %1
    jmp trap_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 128
isr_syscall equ isr128

trap_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp
    mov r12, rsp
    and rsp, -16
    call trap_dispatch
    mov rsp, r12

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; int64_t usermode_run(entry, user_sp, a0, a1, a2)
; Drops to ring 3 at `entry` with rdi=a0 rsi=a1 rdx=a2. Returns when the
; program exits or faults (usermode_abort).
global usermode_run
usermode_run:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rel kernel_resume_rsp], rsp

    mov r9, rdx
    mov r10, rcx
    mov r11, r8

    mov ax, 0x1B
    mov ds, ax
    mov es, ax

    push qword 0x1B         ; ss  (user data, RPL 3)
    push rsi                ; rsp
    push qword 0x2          ; rflags: IF=0, IOPL=0
    push qword 0x23         ; cs  (user code, RPL 3)
    push rdi                ; rip

    mov rdi, r9
    mov rsi, r10
    mov rdx, r11
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; void usermode_abort(int64_t status): back into usermode_run's caller.
global usermode_abort
usermode_abort:
    mov rsp, [rel kernel_resume_rsp]
    mov rax, rdi
    mov cx, 0x10
    mov ds, cx
    mov es, cx
    mov ss, cx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

section .rodata
align 8
global isr_table
isr_table:
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq isr_syscall

section .bss
align 8
kernel_resume_rsp: resq 1

section .note.GNU-stack noalloc noexec nowrite progbits
