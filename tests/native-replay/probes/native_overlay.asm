; Relocated A/B/A at one address; expect OVL1, AAAA/BBBB/AAAA and 49 (little-endian).
bits 16
org 100h
start:
    push cs
    pop ds
    cld
    mov ax, cs
    add ax, (overlay_space-$$+100h) / 16
    mov [parameters], ax
    mov [parameters+2], ax
    mov [entry+2], ax
    mov ax, 13h
    int 10h

    finit
    fild word [seven]
    fmul st0, st0
    fistp word [answer]
    cmp word [answer], 49
    jne failed
    mov dx, name_a
    call overlay
    cmp ax, 0aaaah
    jne failed
    mov [results], ax
    mov dx, name_b
    call overlay
    cmp ax, 0bbbbh
    jne failed
    mov [results+2], ax
    mov dx, name_a
    call overlay
    cmp ax, 0aaaah
    jne failed
    mov [results+4], ax

    mov dx, output_name
    xor cx, cx
    mov ah, 3ch
    int 21h
    jc failed
    mov bx, ax
    mov dx, output
    mov cx, output_end-output
    mov ah, 40h
    int 21h
    jc failed
    cmp ax, output_end-output
    jne failed
    ; Leave the file open to test flushing during DOS termination.
    mov ax, 4c00h
    int 21h
failed:
    mov ax, 4c01h
    int 21h

overlay:
    push cs
    pop es
    mov bx, parameters
    mov ax, 4b03h
    int 21h
    jc failed
    call far [entry]
    push ax
    ; Hold each overlay/raster state for a full BIOS tick.
    xor ah, ah
    int 1ah
    mov bx, dx
.wait:
    xor ah, ah
    int 1ah
    cmp dx, bx
    je .wait
    pop ax
    ret

parameters: dw 0, 0
entry: dw 0, 0
seven: dw 7
output: db 'OVL1'
results: times 3 dw 0
answer: dw 0
output_end:
name_a: db 'ONE.OVL', 0
name_b: db 'TWO.OVL', 0
output_name: db 'RESULT.BIN', 0
align 16, db 0
overlay_space: times 512 db 0
