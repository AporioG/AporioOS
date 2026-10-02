[bits 32]
[org 0x400000]

start:
    ; Системный вызов #3: sys_fork()
    mov eax, 3
    int 0x80

    cmp eax, 0
    je .child_process

.parent_process:
    ; В родителе EAX содержит PID ребенка
    mov ebx, msg_parent
    mov esi, 160            ; Родитель живет 160 тиков
    mov edi, 77             ; Код завершения родителя = 77
    jmp .worker_loop

.child_process:
    ; В ребенке EAX равен 0
    mov ebx, msg_child
    mov esi, 80             ; Ребенок живет 80 тиков
    mov edi, 42             ; Код завершения ребенка = 42

.worker_loop:
    mov edx, 0
.loop:
    inc edx

    ; Системный вызов #1: sys_print
    mov eax, 1
    mov ecx, edx
    push edx
    push ebx
    and edx, 3
    mov dl, [spin_chars + edx]
    int 0x80
    pop ebx
    pop edx

    mov ecx, 25000
.delay:
    dec ecx
    jnz .delay

    cmp edx, esi
    jl .loop

    ; Системный вызов #2: sys_exit(EDI)
    mov eax, 2
    mov ebx, edi
    int 0x80

.hang:
    jmp .hang

msg_parent:
    db "Parent Task [Forked] ", 0
msg_child:
    db "Child  Task [Forked] ", 0
spin_chars:
    db "|/-\"
