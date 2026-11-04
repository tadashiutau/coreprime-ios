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
2:  adr x3, _start
    adr x4, loader_pa
    str x3, [x4]
    adr x5, loader_sp_saved
    mov x6, sp
    str x6, [x5]
    mov x0, x19
    bl loader_main
3:  wfe
    b 3b

/* Kernel return entry (the address patched into XNULDRST slot 1).
 * The kernel arrives here at a physical address with x0-x3 = reason/log
 * arguments, but its SP may be an unmapped kernel virtual address and the
 * MMU/caches may be either state (the kernel-side macro only clears M).
 * Restore our own stack and a clean MMU-off/caches-off state before any
 * compiled code (a C prologue would push onto the incoming SP and fault). */
.global xnu_return_entry
xnu_return_entry:
    adr x9, loader_sp_saved
    ldr x10, [x9]
    cbz x10, 1f
    mov sp, x10
1:  mrs x11, sctlr_el1
    bic x11, x11, #1
    bic x11, x11, #4
    bic x11, x11, #0x1000
    msr sctlr_el1, x11
    isb
    ic iallu
    dsb sy
    isb
    b xnu_return_stub
.macro vent slot
    .balign 0x80
    mov x0, #\slot
    mrs x1, esr_el1
    mrs x2, elr_el1
    mrs x3, far_el1
    b loader_exception
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
