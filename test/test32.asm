bits 32

    mov dx, 03F8h
    mov esi, hello
out_loop:
    mov al, [esi]
    test al, al
    jz echo_loop
    out dx, al
    inc esi
    jmp out_loop

echo_loop:
    in al, dx
    out dx, al
    jmp echo_loop

hello:
    db "Hello, world!", 10, 0
