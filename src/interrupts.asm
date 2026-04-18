bits 32

section .text
global interrupt_ignore_stub
global irq0_stub
global irq1_stub

extern irq0_handler_c
extern irq1_handler_c

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