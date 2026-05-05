# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**drumdrum** is a DFAM-style 8-step sequencer program card for the Music Thing Modular Workshop Computer. It runs on an RP2040 (Cortex M0+) using the ComputerCard header-only C++ library at a fixed 48 kHz sample rate.

Three control surfaces share one sequencer state:
- **Panel** — three knobs, switch, and six LEDs on the card itself.
- **Monome Grid** (16×8) over USB host on the front jack.
- **Browser WebMIDI editor** (`editor.html`) over USB device when the card is plugged into a computer.

The Grid vs browser choice is made once at boot from the USB-C CC pins (`USBPowerState()`); a power cycle is required to switch.

The `WORKSHOP_COMPUTER_AI_DIRECTIVE.md` file in this repo is the authoritative reference for platform constraints, API details, and coding standards. Read it before making changes.

## Build

Requires the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) with `PICO_SDK_PATH` set.

```bash
mkdir build && cd build
cmake ..
make
```

Output: `build/drumdrum.uf2` — flash to Workshop Computer by holding BOOT and pressing RESET, then drag the UF2 onto the USB drive.

## Architecture

`DFAMSequencer` subclasses `ComputerCard` and overrides `ProcessSample()`, which runs in an ISR on **Core 0** at 48 kHz and must complete within ~20 μs. **Core 1** owns the USB stack — either TinyUSB host (Grid) or device (WebMIDI), decided once at boot.

Both cores read and write the same `SharedState` struct (`shared_state.h`) — a flat plain-data global with single-byte and naturally aligned 32-bit fields. Cross-core access is atomic on the M0+; no locks or FIFOs needed for state itself. `tickEpoch` is the cross-core "something changed" signal: Core 0 increments it on every step advance and other interesting events; Core 1 polls it to drive Grid LED redraws and SysEx tick notifications.

**Source files:**

| File | Purpose |
|---|---|
| `main.cpp` | `DFAMSequencer` + audio ISR + USB-role selection |
| `shared_state.h` | `SharedState` struct, `gState` extern |
| `usb_core1.cpp/.h` | Core 1 entry — picks Grid loop or device loop |
| `tusb_config.h` | TinyUSB config (dual-role, MIDI device + CDC/FTDI host) |
| `usb_descriptors.c` | USB MIDI device descriptor (VID 0x2E8A / PID 0x10C2, "DrumDrum") |
| `monome_mext.c/.h` | Monome serial protocol (vendored from MLRws) |
| `grid_ui.cpp/.h` | drumdrum-specific Grid layout + key dispatch |
| `midi_sysex.cpp/.h` | SysEx parser + outbound state push |
| `editor.html` | Self-contained browser editor (React + Babel from CDN) |

**Key constraints:**
- All arithmetic in `ProcessSample` is `int32_t` — no float, no division. Multiply + shift only.
- `ProcessSample` runs in interrupt context. No allocations, no blocking, no prints.
- System clock is 144 MHz (set before ComputerCard construction to reduce ADC artifacts).
- `copy_to_ram` binary type eliminates flash cache jitter.
- `sleep_ms(150)` at the start of the constructor matches MLRws's known-good init sequence — without it the USB controller doesn't always come up cleanly after reset.

## USB role selection (boot-time)

`USBPowerState()` reads the USB-C CC pins on Rev 1.1+:

| Reading | Meaning | Mode |
|---|---|---|
| `UFP` | Plugged into a computer | Device — WebMIDI editor |
| `DFP` | Powering a peripheral | Host — Monome Grid |
| `Unsupported` | Older hardware | Host (default) |

Init pattern matches MLRws (which is the reference known-working setup on this hardware):
- **Device mode** — `board_init()` + `tud_init(0)` are called from Core 0 in `main()` before launching Core 1, so the host can enumerate immediately. Core 1 just runs `tud_task()` + `midi_device_task()`.
- **Host mode** — Core 1 calls `board_init()` + `tusb_init()` (the dual-role-aware init) itself, then runs `mext_task()` + `grid_ui_*()`.

Do **not** probe-then-switch (initialize one stack, wait, tear down, init the other). The remote host sees a brief device that vanishes and gives up enumerating.

## I/O Map

| Jack | Function |
|------|----------|
| CV Out 1 | VCO 1 pitch (calibrated 1V/oct via `CVOut1MIDINote`) |
| CV Out 2 | Velocity CV (summed with CV In 1 for decay modulation) |
| Audio Out 1 | White noise (xorshift32 PRNG, always running) |
| Audio Out 2 | VCO 2 pitch (uncalibrated, ~28.4 DAC units/semitone) |
| Pulse Out 1 | Step trigger (~2 ms pulse) / edit preview trigger when paused |
| Pulse Out 2 | End-of-cycle trigger |
| Pulse In 1 | External clock (overrides internal tempo) |
| Pulse In 2 | Reset to step 1 |
| CV In 1 | Decay CV mod |
| CV In 2 | Global pitch transpose (±24 semitones) |

## Mode Behaviour (panel)

- **Switch UP (play):** Main=tempo, X=sequence length (1–8), Y=VCO 2 pitch offset (±24 semitones). LEDs show playback step.
- **Switch MIDDLE (edit):** Main=tempo, X=step pitch (full 0–127, pickup), Y=step velocity (0–255, pickup). LEDs show edit cursor. Playback continues independently.
- **Switch DOWN (momentary):** Short press (<500 ms) advances edit cursor. Long press (≥500 ms) toggles play/pause. Preview trigger fires when cursor moves while paused.

## Grid layout (16×8)

**Left half (cols 0–7) — sequence overview:**
- Row 0: length selector (cols 0..N-1 lit at brightness 8 for length N).
- Rows 1–7: per-step bar — *height* = pitch, *brightness* = velocity. Current playback step = brightness 15. Edit step has a brightness floor of 6.

**Right half (cols 8–15) — selected-step editor:**
- (col 15, row 0): play/pause toggle (15 playing, 3 paused).
- Rows 1–5: pitch picker — 40 cells covering the full 0..127 MIDI range as 3-or-4-pitch bins. Bottom-left = lowest pitch, top-right = highest. Tapping snaps the pitch to the bin centre; the panel knob can dial anything in between for fine edits.
- Rows 6–7: 16-cell velocity bar. Bottom-left = lowest, top-right = highest. Cells fill row 7 first (left-to-right), then row 6.

The grid display refreshes when `SharedState` differs from a snapshot the renderer keeps locally — typically driven by `tickEpoch` increments and key events.

## SysEx protocol (browser editor)

Manufacturer ID `0x7D`. All messages framed as `F0 7D <cmd> <payload> F7`.

**Inbound (browser → card):**
- `0x01` set step pitch — `(step, pitch)`
- `0x02` set step velocity — `(step, v_hi, v_lo)` where `v = (hi<<4) | lo`
- `0x03` set sequence length — `(length)`
- `0x04` set play/pause — `(playing)`
- `0x05` request full dump

**Outbound (card → browser):**
- `0x10` full dump (28-byte payload: cmd, length, playing, currentStep, 8 pitches, 8 × 2-byte velocity nibbles)
- `0x11` tick — `(currentStep)`, sent on every `tickEpoch` change
- `0x12` parameter update — mirror of `0x01..0x04` for panel-knob changes pushed to the browser

`midi_device_task()` polls `tickEpoch` and a per-field "mirror" snapshot to detect changes from any source, so panel-knob edits propagate to the browser the same way Grid taps would.

## Browser editor (`editor.html`)

Single self-contained HTML file. React 18 + Babel are loaded from `unpkg.com` so the design (lifted from a Claude Design handoff) can run with minimal porting and the file stays openable from `file://`. Trade-off: needs internet on first open. State lives in a `usePattern()` hook that mirrors `SharedState` and fans setters out to outbound SysEx; inbound `FULL_DUMP`, `TICK`, and `PARAM_UPDATE` messages drive React state so the playhead and panel-knob edits both stay live. Slider/scrubber components use an explicit `dragging` ref pattern — never `__move` properties, which cause sticky-drag bugs.

## Key Design Decisions

- **Pickup/catchup knobs:** In edit mode, X (pitch) and Y (velocity) knobs don't write the stored value until the knob comes within ±3 of it. After an external edit (Grid tap, browser change) or an editStep change, pickup is reset *and* the knob's raw position at reset is stashed — pickup can only re-catch once the knob has actually moved at least `KNOB_MOVE_THRESH` (60 ADC counts ≈ 1.5%). Without the movement requirement, parking the knob anywhere near the new value would silently overwrite Grid/browser edits next sample. The same guard applies to the play-mode length knob.
- **Tempo curve:** Quadratic mapping `1600 + (inv² × 6) >> 10` gives ~30 steps/s to ~2 s/step. Musical feel, no float.
- **Audio Out 2 pitch scaling:** `(note - 60) * 1820 >> 6` approximates 1V/oct on the 12-bit audio DAC. Not EEPROM-calibrated — tune VCO 2 by ear.
- **LED encoding steps 1–8:** Fill LEDs 0–5 sequentially for steps 1–6; steps 7–8 drop the top LEDs. See `ledPattern[]` array.
- **External clock:** `Connected(Input::Pulse1)` via normalisation probe detects patching. Internal tempo keeps updating in background so unplugging resumes smoothly.
- **Boot mute:** Audio and pulse outputs are held at zero for the first 150 ms after power-on so settling DACs and immediate startup state can't make a click. Step 1's trigger fires (but EOC does not) the moment the mute lifts.
- **Pitch bin mapping:** Grid pitch picker uses `cell = pitch * 40 / 128` for render and `pitch = (cell * 128 + 64) / 40` for tap (bin centre). Every MIDI pitch lands in exactly one cell.
- **State sharing:** All cross-core writes are direct to `gState`. Single-byte stores are atomic on M0+; multi-byte fields use natural alignment + `volatile`. The only "FIFO" is the mext key-event ring buffer inside `monome_mext.c`.
