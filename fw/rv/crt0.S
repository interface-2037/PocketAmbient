/* crt0.S - reset entry: stack, zero .bss (also on a CPU reset without a reload), main. */
    .section .text.start, "ax"
    .globl _start
_start:
    la    sp, _stack_top
    la    t0, _bss_start
    la    t1, _bss_end
1:  bgeu  t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:  call  main
3:  j     3b
