bits 64

    mov dx, 03F8h
    mov rsi, hello
out_loop:
    mov al, [rsi]
    test al, al
    jz echo_loop
    out dx, al
    inc rsi
    jmp out_loop

echo_loop:
    in al, dx
    out dx, al
    jmp echo_loop

hello:
    db "Hello, world!", 10, 0
