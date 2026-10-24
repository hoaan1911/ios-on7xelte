ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : { KEEP(*(.text.head)) *(.text*) }
    .rodata : { *(.rodata*) }
    /* bss is stored as zeros so the raw Image covers every byte up to
     * _image_end; herolte-pack-boot.py --append places the payload there. */
    .data : { *(.data*) *(.bss*) *(COMMON) }
    . = ALIGN(4096);
    _image_end = .;
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
