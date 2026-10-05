# bluebox eurorack custom firmware

> [!WARNING]
> **Install at your own risk.** This is unofficial custom firmware, not made or supported by 1010music. It hasn't
> damaged a device so far, but any custom firmware carries some risk of bricking your module, losing settings or
> projects, or behaving unexpectedly, and installing it may affect your warranty. The authors accept no
> responsibility for any damage. Keep your stock firmware file and back up your SD card before installing.

Unofficial firmware mod for the 1010music **bluebox eurorack edition** (not the desktop bluebox; their firmware
differs). It patches the official firmware, version 3, and adds features to it. Everything stock still works.

The CPU meter and the new reverb styles run on a real unit; the reverb's Size knob, Freeze position and knob memory
and the delay additions are tested in an emulator so far. Keep your stock firmware file so you can go back at any
time.

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
- **Delay:** the stock **Cutoff** and **Width** band-pass (on with **FILT**) plus:
  - **Pitch** (with a **PITCH** on/off button): shifts each repeat by -12 to +12 semitones, like speeding up or
    slowing down tape, so with Feedback every repeat climbs or falls again (+12: an octave, two octaves, ...). With
    PING on, every bounce, left or right, is one more step.
  - **Flutter**: a slow tape-style pitch wobble on the repeats.
  - **Send**: feeds the echoes into the reverb.

  BEAT and PING sit in a column to the left of the knobs; FILT is under Send, and PITCH and QUAD (3&4 mode) are in
  the column on the right.
  Source: [`src/dly_m7.h`](src/dly_m7.h) (audio), [`src/dly_m4.c`](src/dly_m4.c) (new params).

The panels, two columns per encoder page:

| panel | page 1 | page 2 | page 3 |
|---|---|---|---|
| Reverb | Time, Level, Diffusion, Spread | Size, Pre Delay, Low Cut, HI C | Freeze |
| Delay | Delay, Feedback, Cutoff, Width | Pitch, Flutter, Send, FILT | BEAT, PING, PITCH, QUAD (3&4 mode) |

Good to know:
- Switching to or from one of the new styles mutes the reverb for a moment while its memory is cleared.
- On MVerb, Size takes effect when you stop turning the knob, with a short dropout.
- A project saved with one of the new styles won't load that style on stock firmware.
- Cutoff runs from 80 Hz to 10 kHz and Width from half an octave to 8 octaves, as stock.
- Pitch is plain resampling: the delay reads its memory faster or slower and jumps back every delay time (at most
  every 100 ms) with a short crossfade. So pitched repeats land up to 100 ms early or late, going up repeats a little
  of the sound and going down skips a little. With Pitch at 0 or PITCH off the repeats are untouched.
- Flutter adds up to a few milliseconds to the delay time. The delay's send reaches the reverb 0.7 ms late (the
  reverb runs first).

## Install

You need your own copy of the official bluebox eurorack firmware **version 3** from 1010music (the mod can't include
it), and Python 3 (built into macOS; on Windows get it from python.org).

1. Download [`releases/bluebox-mod-v0.8-patcher.py`](https://github.com/lexnels/1010-bluebox-euro-custom-fw/raw/main/releases/bluebox-mod-v0.8-patcher.py)
   (the newest patcher in the [`releases`](releases) folder).
2. In a terminal, run it on your stock firmware file:
   ```
   python3 bluebox-mod-v0.8-patcher.py "path/to/BLUEEURO 3.BIN"
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

## The delay additions in detail

The stock delay (FX1: vtable `0x0806ba64`, bus 13, wet only) reads its delayed signal, runs a band-pass on it when
Filt is on, feeds it back into its lines and copies it to its bus. `src/dly_m7.h` hooks that path:

- `dly_process` (vtable slot `0x0806ba70`) picks up the new params from the event queue (the stock loop ignores
  unknown ids), runs the stock body, then keeps Send x the wet output.
- `dly_read` (all nine line reads, `FUN_08059d54`: the output reads plus the in-loop reads of PING and QUAD+PING)
  keeps the stock read and its glide after a time change, then re-reads the same line a variable amount further
  back: Flutter, while the line is steady, a 0.6 Hz wobble plus a slow random drift (up to 2 x 2.5 ms); Pitch, a
  read that slides at the pitch ratio and jumps back every delay time (at most 4800 samples) with a 240-sample
  crossfade. Each line read shifts once; in PING mode line C (2T, carrying the right side) shifts twice, since one
  pass through it spans two of the left side's.
- The reverb runs before the delay in each block, so `hall_process` adds the kept send into the reverb's bus (14) at
  the start of the next block.

New ids, free in both cores' tables: `0x3a` Flutter, `0x3d` Send (0..1000), `0x4a` Pitch (-12..12, a plain number, type 1), `0x4b` Pitch on/off.
`src/dly_m4.c` defines them in the M4 param table (hooking its last definition, `bl` at `0x0813714a`), the delay's
param list (`FUN_0812060c` case 4, `0x08120bba`) is a table like the reverb's, and the delay panel pages like the
reverb's instead of being pinned to page 1.

The FX panel (`FUN_0812bc60` populate, `FUN_0812c110` layout) draws 10 widgets in 5 columns of 2, from 48 px in, and
hides the rest. Its "hide past 10" check (`0x0812bede`) now allows 12, and the calls to populate and layout go through
wrappers in `src/dly_m4.c` that move four of the delay's widgets: FILT under Send, BEAT and PING into the empty 48 px
column on the left, PITCH and QUAD into column 5. Other panels get the stock positions. State lives in backup SRAM at `0x38800d00`.

## Build

Needs `arm-none-eabi-gcc` and Python 3. Put your stock image at `firmware/BLUEEURO-3.bin`
(SHA-256 `fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b`).

```
./build.sh
python3 patch.py cpu          # -> out/cpu/BLUEEURO.BIN
python3 patch.py cpu hall     # -> out/cpu+hall/BLUEEURO.BIN (patchsets combine)
python3 patch.py cpu hall delay   # -> out/cpu+hall+delay/BLUEEURO.BIN (the full mod; delay needs hall)
python3 test_cpu.py           # runs both hooks under Unicorn (pip install unicorn capstone)
python3 test_hall.py          # hall hooks under Unicorn: style switching, bypass, memory hand-over, cost
cc -O2 -o out/hall_host tests/hall_host.c -lm && out/hall_host   # hall DSP on the host: decay times, stereo, freeze
cc -O2 -o out/rev_host tests/rev_host.c -lm && out/rev_host      # MVerb, Squall, Freeverb on the host
python3 test_delay.py         # delay hooks on the real stock delay under Unicorn: panel layout, echoes, pitch, flutter, send, cost
python3 tools/make_patcher.py cpu+hall+delay v0.8   # -> out/release/bluebox-mod-v0.8-patcher.py (copy it to releases/)
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
