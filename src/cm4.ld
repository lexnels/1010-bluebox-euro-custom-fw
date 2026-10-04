/* M4 code cave (the M4 can execute from bank 1): in the unused, zero-filled flash gap inside the main image (file 0x4ac00-0xa0000). */
MEMORY { CAVE (rx) : ORIGIN = 0x080B8000, LENGTH = 0x8000 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
