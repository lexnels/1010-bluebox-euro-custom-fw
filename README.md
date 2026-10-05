# bluebox eurorack custom firmware

Unofficial firmware mod for the 1010music **bluebox eurorack edition** (not the desktop bluebox; their firmware
differs). It patches the official firmware, version 3, and adds features to it. Everything stock still works.

The CPU meter and the new reverb styles run on a real unit; the Size knob, Freeze position and knob memory are tested
in an emulator so far. **Use at your own risk.** This isn't made or supported by 1010music. Keep your stock firmware
file so you can go back at any time.

## What it adds

- **CPU meter:** a thin bar at the top right of the screen, right of the clock, showing the audio engine's load.
  Green, amber above 70 %, red above 90 %; a white tick marks the recent peak.
- **Four new reverb styles**, after the stock 15:
  - **FDN Hall**: a lush, smooth hall built for this mod (an 8-line modulated feedback delay network).
    Source: [`src/hall_dsp.h`](src/hall_dsp.h).
  - **MVerb**: a dense plate/hall, ported from Martin Eastwood's [MVerb](https://github.com/martineastwood/mverb).
    Port: [`src/rev_mverb.h`](src/rev_mverb.h).
  - **Squall**: the Mutable Instruments Clouds reverb, as voiced in
    [Squall](https://github.com/DanielMajid/squall_reverb). Port: [`src/rev_squall.h`](src/rev_squall.h).
  - **Freeverb**: the classic [Freeverb](https://github.com/sinshu/freeverb).
    Port: [`src/rev_freeverb.h`](src/rev_freeverb.h).

  The three ports share a wrapper, [`src/rev_common.h`](src/rev_common.h), for low cut, pre-delay, width and level.
- **More reverb controls on the panel:** **Diffusion**, **Spread** and **Size** knobs, with **Freeze** moved to its
  own column on the right. Diffusion and Spread also work on the stock styles; Size works on the four new styles.
- **Each reverb style remembers its knobs.** Switch away and back, and its settings (and the knobs on screen) come
  back. Until power-off.

The reverb panel, two columns per encoder page:

| page 1 | page 2 | page 3 |
|---|---|---|
| Time, Level, Diffusion, Spread | Size, Pre Delay, Low Cut, HI C | Freeze |

Good to know:
- Switching to or from one of the new styles mutes the reverb for a moment while its memory is cleared.
- On MVerb, Size takes effect when you stop turning the knob, with a short dropout.
- A project saved with one of the new styles won't load that style on stock firmware.

## Install

You need your own copy of the official bluebox eurorack firmware **version 3** from 1010music (the mod can't include
it), and Python 3 (built into macOS; on Windows get it from python.org).

1. Download [`releases/bluebox-mod-v4-patcher.py`](https://github.com/lexnels/1010-bluebox-euro-custom-fw/raw/main/releases/bluebox-mod-v4-patcher.py)
   (the newest patcher in the [`releases`](releases) folder).
2. In a terminal, run it on your stock firmware file:
   ```
   python3 bluebox-mod-v4-patcher.py "path/to/BLUEEURO 3.BIN"
   ```
   It checks that the file is the right stock firmware, then writes `BLUEEURO.BIN` next to the patcher.
3. Copy `BLUEEURO.BIN` to the root of the microSD card and install it the way you'd install a 1010music update.

**Going back to stock:** install your stock firmware file the same way.

## Credits and licences

- MVerb by Martin Eastwood ([martineastwood/mverb](https://github.com/martineastwood/mverb)), **GPL-3.0**. Its port is
  kept in its own file, `src/rev_mverb.h`, under that licence.
- Squall by Daniel Majid Mirzakhani ([DanielMajid/squall_reverb](https://github.com/DanielMajid/squall_reverb)); the
  reverb core is by Emilie Gillet (Mutable Instruments), MIT.
- Freeverb by Jezar at Dreampoint, public domain ([sinshu/freeverb](https://github.com/sinshu/freeverb)).
- The patching approach follows [j3threejay/blackbox-mod](https://github.com/j3threejay/blackbox-mod).
- The stock firmware is 1010music's property and is not included here. The patchers in `releases/` only
  carry the mod's own changes.

---

# Technical notes

## The reverb styles in detail

Four reverb styles after the stock 15 (Clouds), each with its own engine running in the stock reverb's delay memory:

| style | engine | source |
|---|---|---|
| FDN Hall | pre-delay, a 3-stage 8-channel diffuser, then an 8-line feedback delay network with modulated lines, two-band decay, damping and an allpass in each line, mixed by an 8x8 Hadamard matrix (`src/hall_dsp.h`) | written for this mod |
| MVerb | Dattorro-style plate/hall tank (`src/rev_mverb.h`) | port of [martineastwood/mverb](https://github.com/martineastwood/mverb), **GPL-3.0** |
| Squall | the Mutable Instruments Clouds reverb as voiced in Squall (`src/rev_squall.h`) | [DanielMajid/squall_reverb](https://github.com/DanielMajid/squall_reverb); reverb core by Emilie Gillet, MIT |
| Freeverb | 8 combs + 4 allpasses per side (`src/rev_freeverb.h`) | [sinshu/freeverb](https://github.com/sinshu/freeverb), Jezar's public-domain original |

MVerb, Squall and Freeverb share a wrapper (`src/rev_common.h`) that adds the low cut, pre-delay, stereo width, level
and a fade-in they lack.

What each control does per style:

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
Pre Delay, Diffusion and Spread are the stock engine's own params; Size (`0x143`) is only used by the new styles and
never reaches the stock engine, whose styles set their own size.

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
python3 tools/make_patcher.py cpu+hall v4   # -> out/release/bluebox-mod-v4-patcher.py (copy it to releases/)
```

Never commit firmware images: `*.bin` / `*.BIN` are git-ignored.

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
