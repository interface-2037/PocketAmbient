/* link.ld - composer firmware in the 64 KB CPU BRAM (docs/REGISTER_MAP.md). The image is
 * loaded as-is (BRAM init or SD data slot) and is never reloaded on a CPU reset, so the
 * firmware must not depend on initialised writable data: .data must stay empty. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K }
STACK_SIZE = 4K;
SECTIONS
{
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > RAM
    .data : { *(.data .data.* .sdata .sdata.*) } > RAM
    _image_end = .;
    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM
    .stack (NOLOAD) : ALIGN(16) { . += STACK_SIZE; _stack_top = .; } > RAM
    ASSERT(SIZEOF(.data) == 0, "firmware must not use initialised writable data (see link.ld)")
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) }
}
