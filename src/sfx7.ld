/* M7 cave for the send FX (chorus, drive, delay 2), after the USB cave, below the master bus cave in the same zero-filled flash gap. */
MEMORY { CAVE (rx) : ORIGIN = 0x08098000, LENGTH = 0x8000 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
