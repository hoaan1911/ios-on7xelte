ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : { KEEP(*(.text.head)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(4096);
    _image_end = .;
    _image_size = ABSOLUTE(_image_end) + 0x10000;
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.note*) *(.comment*) }
}
