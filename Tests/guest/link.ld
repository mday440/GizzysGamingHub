ENTRY(_start)
SECTIONS {
  . = 0x8000000;
  .text : { *(.text._start) *(.text*) }
  . = ALIGN(0x1000);
  .rodata : { *(.rodata*) }
  . = ALIGN(0x1000);
  .data : { *(.data*) *(.got*) }
  .bss : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
