bits 16
org 100h
    mov ax, 13h
    int 10h
    mov al, 0b6h
    out 43h, al
    mov ax, 1193
    out 42h, al
    mov al, ah
    out 42h, al
    in al, 61h
    or al, 3
    out 61h, al
    xor ah, ah
    int 1ah
    mov bx, dx
.wait:
    xor ah, ah
    int 1ah
    cmp bx, dx
    je .wait
    mov ax, 4c07h
    int 21h
