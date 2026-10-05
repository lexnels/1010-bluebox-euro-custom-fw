# 1010-bluebox-euro-custom-fw

Custom firmware modifications for the 1010music **bluebox eurorack edition**
(not the desktop bluebox; firmware images differ between the two).

## Goals

- Add a **CPU meter** to the UI. Done, works on hardware.
- Add a new lush, creamy **hall reverb**: build `hall`, "FDN Hall" (first called Lush Hall) as a 16th reverb style.
  Works on hardware.
- Alternative reverb styles **MVerb**, **Squall** and **Freeverb**, and **Diffusion** and **Spread** knobs on the reverb
  panel: build `hall`. Works on hardware.
- **Size** knob, Freeze moved to the right of the knobs, and every style remembering its own knob settings: build
  `hall`. Untested on hardware.

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

Four reverb styles after the stock 15 (Clouds), each with its own engine running in the stock reverb's delay memory:

| style | engine | source |
|---|---|---|
| FDN Hall | pre-delay, a 3-stage 8-channel diffuser, then an 8-line feedback delay network with modulated lines, two-band decay, damping and an allpass in each line, mixed by an 8x8 Hadamard matrix (`src/hall_dsp.h`) | written for this mod |
| MVerb | Dattorro-style plate/hall tank (`src/rev_mverb.h`) | port of [martineastwood/mverb](https://github.com/martineastwood/mverb), **GPL-3.0** |
| Squall | the Mutable Instruments Clouds reverb as voiced in Squall (`src/rev_squall.h`) | [DanielMajid/squall_reverb](https://github.com/DanielMajid/squall_reverb); reverb core by Emilie Gillet, MIT |
| Freeverb | 8 combs + 4 allpasses per side (`src/rev_freeverb.h`) | [sinshu/freeverb](https://github.com/sinshu/freeverb), Jezar's public-domain original |

MVerb, Squall and Freeverb share a wrapper (`src/rev_common.h`) that adds the low cut, pre-delay, stereo width, level
and a fade-in they lack.

**Reverb panel.** The panel lays out its controls in columns of two, and the encoders work on two columns at a time.
It now has 8 knobs and Freeze on its own at the right: Time, Level, **Diffusion**, **Spread** | **Size**, Pre Delay,
Low Cut, HI C | Freeze (a third encoder page). Diffusion and Spread are the stock engine's own params, so they work on
the stock styles too; Size only acts on the four new styles (the stock styles set their own size). Projects and FX
presets save all of them.

**Each style keeps its own knobs.** Leaving a style remembers its knob settings (all but Freeze); coming back puts
them back and the panel's knobs move to them. A style you haven't used since power-on starts from its preset (stock
styles) or the knobs as they were (new styles), with Time and Level at the middle, as the stock UI does. The memory
lasts until power-off.

| control | FDN Hall | MVerb | Squall | Freeverb |
|---|---|---|---|---|
| Time | decay 0.4 s / 3.2 s / 25 s (bottom / middle / top) | decay | loop gain | decay 0.5 s / 2.5 s / 12.5 s |
| Level | output level | output level | output level | output level |
| Diffusion | spread of the input diffuser (smear of the onset) | tank density | allpass amount | allpass feedback |
| Spread | stereo width, -1000 mono, 0 normal, +1000 extra wide | same | same | same |
| Size | room size (delay lengths); the middle is the tuned size; turning it bends pitch briefly | tank size; applied once the knob rests, with a short dropout (MVerb clears its tank) | loop delays x0.5 to x2 | comb lengths x0.5 to x2 |
| Pre Delay | pre-delay, up to ~0.65 s | pre-delay of the tank | pre-delay | pre-delay |
| Low Cut | input high-pass, 20 Hz to 1 kHz | same | same | same |
| HI C | brightness of the input and of the tail | damping and bandwidth | loop low-pass | damping |
| Freeze | holds the tail and ignores new input | same | same | same |

Feedback, modulation, early reflections and Resonance aren't on the panel, so the new styles use fixed, tuned values
for them. Size changes the decay along with the room in MVerb, Squall and Freeverb (Time is quoted at the middle Size).

Switching to or from one of the new styles mutes the reverb for ~30 audio blocks while the shared delay memory is
cleared. A project saved with one of the new styles won't load that style on stock firmware.

**Licence note:** MVerb is GPL-3.0. Its port is kept in its own file, `src/rev_mverb.h`, under that licence.

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
cc -O2 -o out/rev_host tests/rev_host.c -lm && out/rev_host      # MVerb, Squall, Freeverb on the host
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
  `0x0806adac`) is replaced by `hall_process`, which runs the stock engine unless one of styles 15-18 is selected. The setter
  call in the stock event loop (`bl` at `0x08048e56`) goes to `hall_set`, which remembers every reverb knob (in backup
  SRAM at `0x38800000`) and keeps styles 15+ from the stock setter, which only has 15 presets. It also saves and
  restores each style's knobs; `hall_echo` (the style-change echo, `bl` at `0x08048e6e`) then sends every panel knob
  back to the M4. The new engines use the stock engine's 1 MB delay memory, which is zeroed whenever it changes hands.
  Both cores' style name lists are copied with the new names and their count raised to 19. On the M4, the reverb
  slot's parameter list (`FUN_0812060c` case 5, `0x08120c1c`), which decides the panel's knobs, what presets save and
  what is re-sent to the M7, is rewritten as a table, and the panel's encoder pages for this slot go to 3.

`ghidra/` has headless import/decompile scripts (Ghidra 11.4.2 + its bundled pyghidra). `tools.py` has address and
branch helpers.

## Hardware status

- Build `cpu` flashed and works on a real unit (2026-10-04). The bootloader accepts a modified image of the same size,
  and the screen's memory rows run top-down as assumed.
