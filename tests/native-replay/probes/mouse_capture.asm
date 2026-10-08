bits 16
org 100h
start:
    push cs
    pop ds
    mov ax, 13h
    int 10h
    xor ax, ax
    int 33h
    mov [state], ax
    mov ax, 1ah
    mov bx, 50
    mov cx, 50
    mov dx, 64
    int 33h
poll:
    mov ax, 0bh
    int 33h
    add [state+2], cx
    add [state+4], dx
    jmp poll
state: times 3 dw 0
