bits 16

    mov dx, 03F8h
    mov si, hello
out_loop:
    mov al, [si]
    test al, al
    jz echo_loop
    out dx, al
    inc si
    jmp out_loop

echo_loop:
    in al, dx
    out dx, al
    jmp echo_loop

hello:
    db "Hello, world!", 10, 0
