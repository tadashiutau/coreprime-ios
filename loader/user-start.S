.text
.align 2
.globl _start
_start:
 mov x0, sp
 and sp, x0, #0xfffffffffffffff0
 mov x29, #0
 mov x30, #0
 bl _bringup_main
1: b 1b
