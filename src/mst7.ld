/* M7 cave for the master bus (compressor meter, saturator), below the CPU meter's (0x080B0000) in the same zero-filled flash gap. */
MEMORY { CAVE (rx) : ORIGIN = 0x080A0000, LENGTH = 0x8000 }
SECTIONS {
  .cave : { *(.text .text.* .rodata .rodata.*) } > CAVE
  /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON .ARM.exidx* .comment) }
}
