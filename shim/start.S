.section .text.head,"ax"
.global _head
_head:
    b _start
    .long 0
    .quad 0x80000
    .quad _end
    .quad 0xa
    .quad 0, 0, 0
    .ascii "ARM\x64"
    .long 0
.text
.global _start
_start:
    msr daifset, #0xf
    mov x19, x0
    msr spsel, #1
    adr x1, _stack_top
    mov sp, x1
    adr x1, vectors
    msr vbar_el1, x1
    isb
    adr x1, _bss_start
    adr x2, _bss_end
1:  cmp x1, x2
    b.hs 2f
    str xzr, [x1], #8
    b 1b
2:  mov x0, x19
    bl diag_main
3:  wfe
    b 3b
.macro vent slot
    .balign 0x80
    mov x0, #\slot
    mrs x1, esr_el1
    mrs x2, elr_el1
    mrs x3, far_el1
    b diag_exception
.endm
.balign 0x800
vectors:
    vent 0
    vent 1
    vent 2
    vent 3
    vent 4
    vent 5
    vent 6
    vent 7
    vent 8
    vent 9
    vent 10
    vent 11
    vent 12
    vent 13
    vent 14
    vent 15
