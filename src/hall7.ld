/* M7 cave for the hall reverb, after the CPU meter's (0x080B0000) in the same zero-filled flash gap. */
MEMORY { CAVE (rx) : ORIGIN = 0x080C0000, LENGTH = 0x10000 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
