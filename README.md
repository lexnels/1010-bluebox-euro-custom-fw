# bluebox eurorack custom firmware

An unofficial mod for the 1010music **bluebox eurorack edition**, built on stock firmware `BLUEEURO 3.BIN`. It isn't
affiliated with 1010music. This repository holds patch definitions and new code only. It does not contain 1010music's
firmware or any patched image, so don't commit `.bin` files.

## What's in build `cpu`

- **CPU meter:** a 3-pixel vertical bar at the top right, right of the H:MM:SS clock. It shows the audio engine's load:
  green, amber above 70 %, red above 90 %. A white tick marks the worst block of the last ~0.25 s. The clock label
  is 6 px narrower to make room.

## Build

Needs `arm-none-eabi-gcc` and Python 3. Put your stock image at `firmware/BLUEEURO-3.bin`
(SHA-256 `fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b`).

```
./build.sh
python3 patch.py cpu          # -> out/cpu/BLUEEURO.BIN
python3 test_cpu.py           # runs both hooks under Unicorn (pip install unicorn capstone)
```

## Install / go back

Keep the stock file somewhere safe. To install, copy the patched file to the root of the microSD card under the same
name the official update uses, then install it the way you'd install a 1010music update. To go back to stock, do the
same with the stock file.

## How it works

The file holds three images back to back:

| file offset | flash address | what |
|---|---|---|
| 0x000000 | 0x08040000 | M7 main app: audio engine, USB audio |
| 0x0A0000 | 0x080E0000 | M7 microSD / USB storage mode |
| 0x0C0000 | 0x08100000 | M4 app: screen, UI, SD card, MIDI, presets |

The bootloader at 0x08000000-0x0803FFFF isn't in the file. New code goes in the zero-filled gap in the main image
(0x080B0000 for M7 code, 0x080B8000 for M4 code; the M4 can execute from bank 1), and a few `bl` instructions are
redirected to it. The file size doesn't change.

- **M7** (`src/cm7_cpu.c`): the audio callback `FUN_0804c778` already profiles itself with DWT cycle counts. We replace
  its profiler-exit call (`bl` at `0x0804ca10`), read back the {period, busy} pair it records, and publish the
  average and peak load to backup SRAM (`0x38800f00`).
- **M4** (`src/cm4_cpu.c`): the frame presenter `FUN_081385dc` blits each UI layer into an LTDC back buffer and flips.
  We replace the flip call (`bl` at `0x0813861a`) to draw the bar into the back buffer first.

`ghidra/` has headless import/decompile scripts (Ghidra 11.4.2 + its bundled pyghidra). `tools.py` has address and
branch helpers.

## Untested on hardware

- Whether the bootloader accepts a modified file. It's the same file size, and the Blackbox's bootloader accepts
  modified files.
- Screen orientation: memory rows are assumed to run top-down. If the bar appears at the bottom right instead, change
  `BAR_Y0` in `src/cm4_cpu.c`.
