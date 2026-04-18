bits 32

section .text
global interrupt_ignore_stub
global exception_stub_table
global irq0_stub
global irq1_stub

extern exception_handler_c
extern irq0_handler_c
extern irq1_handler_c

%macro EXCEPTION_STUB_NOERR 1
global exception_stub_%1
exception_stub_%1:
    push dword 0
    push dword %1
    call exception_handler_c
    add esp, 8
    cli
.hang_%1:
    hlt
    jmp .hang_%1
%endmacro

%macro EXCEPTION_STUB_ERR 1
global exception_stub_%1
exception_stub_%1:
    push dword [esp]
    push dword %1
    call exception_handler_c
    add esp, 8
    add esp, 4
    cli
.hang_%1:
    hlt
    jmp .hang_%1
%endmacro

EXCEPTION_STUB_NOERR 0
EXCEPTION_STUB_NOERR 1
EXCEPTION_STUB_NOERR 2
EXCEPTION_STUB_NOERR 3
EXCEPTION_STUB_NOERR 4
EXCEPTION_STUB_NOERR 5
EXCEPTION_STUB_NOERR 6
EXCEPTION_STUB_NOERR 7
EXCEPTION_STUB_ERR 8
EXCEPTION_STUB_NOERR 9
EXCEPTION_STUB_ERR 10
EXCEPTION_STUB_ERR 11
EXCEPTION_STUB_ERR 12
EXCEPTION_STUB_ERR 13
EXCEPTION_STUB_ERR 14
EXCEPTION_STUB_NOERR 15
EXCEPTION_STUB_NOERR 16
EXCEPTION_STUB_ERR 17
EXCEPTION_STUB_NOERR 18
EXCEPTION_STUB_NOERR 19
EXCEPTION_STUB_NOERR 20
EXCEPTION_STUB_ERR 21
EXCEPTION_STUB_NOERR 22
EXCEPTION_STUB_NOERR 23
EXCEPTION_STUB_NOERR 24
EXCEPTION_STUB_NOERR 25
EXCEPTION_STUB_NOERR 26
EXCEPTION_STUB_NOERR 27
EXCEPTION_STUB_NOERR 28
EXCEPTION_STUB_ERR 29
EXCEPTION_STUB_ERR 30
EXCEPTION_STUB_NOERR 31

section .rodata
align 4
exception_stub_table:
    dd exception_stub_0, exception_stub_1, exception_stub_2, exception_stub_3
    dd exception_stub_4, exception_stub_5, exception_stub_6, exception_stub_7
    dd exception_stub_8, exception_stub_9, exception_stub_10, exception_stub_11
    dd exception_stub_12, exception_stub_13, exception_stub_14, exception_stub_15
    dd exception_stub_16, exception_stub_17, exception_stub_18, exception_stub_19
    dd exception_stub_20, exception_stub_21, exception_stub_22, exception_stub_23
    dd exception_stub_24, exception_stub_25, exception_stub_26, exception_stub_27
    dd exception_stub_28, exception_stub_29, exception_stub_30, exception_stub_31

section .text

interrupt_ignore_stub:
    iretd

irq0_stub:
    pusha
    call irq0_handler_c
    popa
    iretd

irq1_stub:
    pusha
    call irq1_handler_c
    popa
    iretd