# 1010-bluebox-euro-custom-fw

Custom firmware modifications for the 1010music **bluebox eurorack edition**
(not the desktop bluebox; firmware images differ between the two).

## Goals

- Add a **CPU meter** to the UI. Done, works on hardware.
- Add a new lush, creamy **hall reverb**: build `hall`, "Lush Hall" as a 16th reverb style. Untested on hardware.

## Approach

Follows the approach of [j3threejay/blackbox-mod](https://github.com/j3threejay/blackbox-mod):
patching the stock firmware image rather than rebuilding from source.

## Note on firmware images

The stock firmware is 1010music's property and is **not** included in this repo.
`*.bin` / `*.BIN` files are git-ignored. Download the official eurorack-edition
firmware from 1010music and keep it locally.

## What's in build `cpu`

- **CPU meter:** a 3-pixel vertical bar at the top right, right of the H:MM:SS clock. It shows the audio engine's load:
  green, amber above 70 %, red above 90 %. A white tick marks the worst block of the last ~0.25 s. The clock label
  is 6 px narrower to make room.

## What's in build `hall`

- **Lush Hall**, a 16th reverb style after Clouds. It runs its own engine (`src/hall_dsp.h`): pre-delay, a 3-stage
  8-channel diffuser (so the onset is a smooth cloud, not discrete echoes), then an 8-line feedback delay network
  with independently modulated delay lines, a two-band decay, damping and an allpass in each line, mixed by an 8x8
  Hadamard matrix.

  The reverb page only shows Style, Time, Level, Pre Delay, Low Cut, HI C and Freeze; the stock engine's other
  params (size, diffusion, modulation and so on) are set inside its presets and never reach the screen. So in Lush
  Hall those are fixed, tuned values, and the visible controls do this:

  | control | in Lush Hall |
  |---|---|
  | Time | decay: 0.4 s at the bottom, 3.2 s in the middle (default), 25 s at the top |
  | Level | output level, like the stock styles |
  | Pre Delay | pre-delay, up to ~0.65 s |
  | Low Cut | input high-pass, 20 Hz to 1 kHz |
  | HI C | tone: brightness of the input and how fast highs die away |
  | Freeze | holds the tail and ignores new input |

  Switching to or from Lush Hall mutes the reverb for ~30 audio blocks while the shared delay memory is cleared.

## Build

Needs `arm-none-eabi-gcc` and Python 3. Put your stock image at `firmware/BLUEEURO-3.bin`
(SHA-256 `fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b`).

```
./build.sh
python3 patch.py cpu          # -> out/cpu/BLUEEURO.BIN
python3 patch.py cpu hall     # -> out/cpu+hall/BLUEEURO.BIN (patchsets combine)
python3 test_cpu.py           # runs both hooks under Unicorn (pip install unicorn capstone)
python3 test_hall.py          # hall hooks under Unicorn: style switching, bypass, memory hand-over, cost
cc -O2 -o out/hall_host tests/hall_host.c -lm && out/hall_host   # hall DSP on the host: decay times, stereo, freeze
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

- **Hall** (`src/hall_m7.c`, cave at `0x080C0000`): the reverb object's process method `FUN_08048de8` (vtable slot
  `0x0806adac`) is replaced by `hall_process`, which runs the stock engine unless style 15 is selected. The setter
  call in the stock event loop (`bl` at `0x08048e56`) goes to `hall_set`, which remembers every reverb knob (in backup
  SRAM at `0x38800000`) and keeps style 15 from the stock setter, which only has 15 presets. In Lush Hall the
  hall uses the stock engine's 1 MB delay memory, which is zeroed whenever it changes hands. Both cores' style name
  lists are copied with a 16th name and their count raised to 16.

`ghidra/` has headless import/decompile scripts (Ghidra 11.4.2 + its bundled pyghidra). `tools.py` has address and
branch helpers.

## Hardware status

- Build `cpu` flashed and works on a real unit (2026-10-04). The bootloader accepts a modified image of the same size,
  and the screen's memory rows run top-down as assumed.
