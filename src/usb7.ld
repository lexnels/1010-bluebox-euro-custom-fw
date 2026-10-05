/* M7 cave for the USB audio out mode, below the master bus cave (0x080A0000) in the same zero-filled flash gap. */
MEMORY { CAVE (rx) : ORIGIN = 0x08090000, LENGTH = 0x8000 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
