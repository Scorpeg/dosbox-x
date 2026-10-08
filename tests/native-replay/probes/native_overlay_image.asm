bits 16
org 0
    dw 5a4dh, 0, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 1ch, 0
    dw relocated_segment-image, 0
image:
    mov ax, cs
    cmp ax, [cs:relocated_segment-image]
    jne failed
    push es
    mov ax, 0a000h
    mov es, ax
    xor di, di
    mov ax, MARKER
    mov cx, 32000
    rep stosw
    pop es
    mov ax, MARKER
    retf
failed:
    xor ax, ax
    retf
relocated_segment: dw 0
    times 512-($-$$) db 0
