[bits 16]
[org 0x7c00]

start:
    jmp 0x0000:.canonical_cs
.canonical_cs:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7c00

    mov [BOOT_DRIVE], dl

    ; Чтение 80 секторов ядра через LBA Packet (INT 13h, AH=42h)
    mov si, disk_address_packet
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    ; --- VESA VBE Переключение (1024x768) ---
    ; Сохраняем ModeInfoBlock по адресу 0x7E00 (прямо за MBR)
    mov ax, 0x4F01
    mov cx, 0x4118          ; 1024x768 LFB (24/32 bpp)
    xor bx, bx
    mov es, bx
    mov di, 0x7E00
    int 0x10
    cmp ax, 0x004F
    jne .try_32bpp
    mov bx, 0x4118
    jmp .set_vbe

.try_32bpp:
    mov ax, 0x4F01
    mov cx, 0x4144          ; 1024x768x32bpp LFB
    mov di, 0x7E00
    int 0x10
    mov bx, 0x4144

.set_vbe:
    mov ax, 0x4F02
    int 0x10

    ; Fast A20 Gate
    in al, 0x92
    or al, 2
    out 0x92, al

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEG:init_pm

disk_error:
    cli
    hlt

[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000
    mov esp, ebp

    call KERNEL_OFFSET
.halt:
    cli
    hlt
    jmp .halt

BOOT_DRIVE db 0
KERNEL_OFFSET equ 0x8000

align 4
disk_address_packet:
    db 0x10
    db 0
    dw 80
    dw 0x8000
    dw 0x0000
    dq 1

align 4
gdt_start:
    dd 0x0, 0x0
gdt_code:
    dw 0xffff, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:
    dw 0xffff, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

times 510 - ($ - $$) db 0
dw 0xaa55
