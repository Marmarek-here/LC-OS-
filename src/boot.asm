; boot.asm — Multiboot2 entry point for LC(OS)
; Assembled with NASM, targets x86_64

bits 32

; ─── Multiboot2 header constants ────────────────────────────────────────────
MULTIBOOT2_MAGIC    equ 0xE85250D6
MULTIBOOT2_ARCH     equ 0           ; i386 protected mode
HEADER_LENGTH       equ (multiboot2_header_end - multiboot2_header_start)
CHECKSUM            equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + HEADER_LENGTH)

section .multiboot2
align 8
multiboot2_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd HEADER_LENGTH
    dd CHECKSUM

    ; end tag
    align 8
    dw 0    ; type
    dw 0    ; flags
    dd 8    ; size
multiboot2_header_end:

; ─── Stack ───────────────────────────────────────────────────────────────────
section .bss
align 16
stack_bottom:
    resb 16384          ; 16 KiB
stack_top:

; ─── Entry point ─────────────────────────────────────────────────────────────
section .text
global _start
extern kernel_main

_start:
    ; Set up stack
    mov esp, stack_top

    ; Clear direction flag, align stack
    cld

    ; Pass Multiboot2 magic/info (eax/ebx) to C entrypoint.
    push ebx
    push eax

    ; Call kernel main
    call kernel_main
    add esp, 8

    ; Hang forever if kernel_main returns
.hang:
    cli
    hlt
    jmp .hang
