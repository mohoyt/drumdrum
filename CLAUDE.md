# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**drumdrum** is a DFAM-style 8-step sequencer program card for the Music Thing Modular Workshop Computer. It runs on an RP2040 (Cortex M0+) using the ComputerCard header-only C++ library at a fixed 48 kHz sample rate.

Four control surfaces share one sequencer state:
- **Panel** — three knobs, switch, and six LEDs on the card itself.
- **Monome Grid** (16×8) over USB host (CDC + FTDI) on the front jack.
- **Music Thing 8mu** over USB host (class-compliant MIDI) on the front jack.
- **Browser WebMIDI editor** (`editor.html`) over USB device when the card is plugged into a computer.

Host vs device is decided once at boot from the USB-C CC pins (`USBPowerState()`); a power cycle is required to switch. Within host mode, Grid vs 8mu is auto-detected from the device's USB class — CDC mount fires `tuh_cdc_*` callbacks (mext) and Audio/MIDIStreaming mount fires our in-tree class driver in `midi_host.cpp`.

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

`DFAMSequencer` subclasses `ComputerCard` and overrides `ProcessSample()`, which runs in an ISR on **Core 0** at 48 kHz and must complete within ~20 μs. **Core 1** owns the USB stack — either TinyUSB host (Grid via CDC, or 8mu via class-compliant MIDI) or device (WebMIDI), decided once at boot. Within host mode, the surface in use is auto-detected from the device's USB class.

Both cores read and write the same `SharedState` struct (`shared_state.h`) — a flat plain-data global with single-byte and naturally aligned 32-bit fields. Cross-core access is atomic on the M0+; no locks or FIFOs needed for state itself. `tickEpoch` is the cross-core "something changed" signal: Core 0 increments it on every step advance and other interesting events; Core 1 polls it to drive Grid LED redraws and SysEx tick notifications. The 8mu has no outbound feedback channel, so it doesn't read `tickEpoch` — it only writes to `gState`.

**Source files:**

| File | Purpose |
|---|---|
| `main.cpp` | `DFAMSequencer` + audio ISR + USB-role selection |
| `shared_state.h` | `SharedState` struct, `gState` extern |
| `usb_core1.cpp/.h` | Core 1 entry — picks host loop or device loop |
| `tusb_config.h` | TinyUSB config (dual-role, MIDI device + CDC/FTDI host + MIDI host hint) |
| `usb_descriptors.c` | USB MIDI device descriptor (VID 0x2E8A / PID 0x10C2, "DrumDrum") |
| `monome_mext.c/.h` | Monome serial protocol (vendored from MLRws) |
| `grid_ui.cpp/.h` | drumdrum-specific Grid layout + key dispatch |
| `midi_host.cpp/.h` | In-tree class-compliant USB MIDI host driver for 8mu |
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
| `DFP` | Powering a peripheral | Host — Grid or 8mu (auto) |
| `Unsupported` | Older hardware | Host (default) |

Init pattern matches MLRws (which is the reference known-working setup on this hardware):
- **Device mode** — `board_init()` + `tud_init(0)` are called from Core 0 in `main()` before launching Core 1, so the host can enumerate immediately. Core 1 just runs `tud_task()` + `midi_device_task()`.
- **Host mode** — Core 1 calls `board_init()` + `tusb_init()` (the dual-role-aware init) itself, then runs `mext_task()` (which pumps `tuh_task()` and dispatches CDC for Grid + the MIDI driver's xfer callbacks for 8mu) + `grid_ui_*()`.

Do **not** probe-then-switch (initialize one stack, wait, tear down, init the other). The remote host sees a brief device that vanishes and gives up enumerating.

### MIDI host driver (8mu)

TinyUSB 0.18 (Pico SDK 2.2.0) ships only `class/midi/midi_device.c` — no MIDI host driver. `CFG_TUH_MIDI` exists as a config flag but only enables a one-block descriptor-parser hint in `usbh.c` that groups class-compliant USB MIDI's Audio-Control + MIDIStreaming interfaces into a single driver claim. We supply the actual driver ourselves in `midi_host.cpp`, registered through TinyUSB's `usbh_app_driver_get_cb()` weak hook.

The driver:
1. Implements `usbh_class_driver_t` (init / deinit / open / set_config / xfer_cb / close).
2. In `open()`, walks descriptors past Audio-Control to find the MIDIStreaming interface, then scans for bulk endpoints and opens them via `tuh_edpt_open`.
3. In `set_config()`, kicks off the first IN read with `usbh_edpt_xfer` and calls `usbh_driver_set_config_complete(dev_addr, TUSB_INDEX_INVALID_8)`.
4. In `xfer_cb()`, parses 32-bit USB-MIDI Event Packets (CIN in low nibble of byte 0; bytes 1–3 are MIDI message), routes Control Change to `handle_cc`, re-arms the IN xfer.

`handle_cc` writes directly to `gState` (single-byte atomic on M0+) and increments `tickEpoch` to nudge other surfaces.

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

## 8mu MIDI mapping

The driver dispatches two CINs from `tuh_midi_rx_cb`: `MIDI_CIN_CONTROL_CHANGE` (0xB) and `MIDI_CIN_NOTE_ON` (0x9). All messages are channel-agnostic.

**Factory 8mu defaults** — work plug-and-play, no 8mu config needed:

| Source                | Message       | Effect                                             |
|-----------------------|---------------|----------------------------------------------------|
| Faders 1–8            | CC 34–41      | step pitches 0–7 raw 7-bit; OR step velocities (`<<1`) when `gState.midiHostVelocityMode` is set |
| Button 1              | Note 36 (C2)  | `gState.midiHostVelocityMode ^= 1`                 |
| Button 2              | Note 48 (C3)  | `gState.playing ^= 1`                              |
| Button 3              | Note 60 (C4)  | `gState.currentStep = 0` (mirrors Pulse In 2 reset) |
| Button 4              | Note 72 (C5)  | `randomize_pattern()` — re-rolls all 8 pitches + velocities using a Core-1-local xorshift seeded from `time_us_32()` |

`note-on` with velocity 0 is treated as note-off (running-status convention) and ignored. Only press edges fire.

**Optional CC alt-bank** — configured by the user in the 8mu web editor for non-default mappings:

| CC      | Effect                                             |
|---------|----------------------------------------------------|
| 22      | (button, edge) toggle pitch ↔ velocity edit mode   |
| 23      | (button, edge) toggle play/pause                   |
| 24      | (button, edge) reset to step 1                     |
| 28      | (fader) `gState.editStep = (value*8)>>7`, clamp 0–7 |
| 50–57   | (faders) step velocities 0–7 (`<<1`), regardless of mode |
| 25–27, 29–33, 42–49, all others | reserved or ignored             |

CC button rising-edge state lives in three file-static `uint8_t`s in `midi_host.cpp` (one per CC 22/23/24); note buttons don't need edge state because each `note-on` event is itself a press. There's no CC equivalent of randomize today — could add CC 25 if useful later.

`gState.midiHostVelocityMode` (single `uint8_t`, written only by the MIDI host driver) toggles the meaning of CC 34–41. CC 50–57 always writes velocities, so an 8mu user can dedicate one bank to pitches and another to velocities and never need to press the toggle.

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
- **8mu mapping rationale:** Faders default to step pitches because 7-bit CC maps 1:1 to MIDI pitch range (no scaling, instantly legible). Velocities (0–255) are reachable via either a button-toggled mode on the same CC range OR a dedicated alt-bank CC range (50–57, `value<<1`); the latter exists so users can dedicate an 8mu bank to velocity without ever touching the toggle. The 8mu's factory buttons send notes 36/48/60/72 (one octave apart), so we listen for those directly and act on `note-on` press edges (velocity > 0). The parallel CC 22–24 alt-bank exists for non-8mu controllers and 8mu users who reconfigured buttons to CC — CC buttons act on rising edge (`value` crossing ≥64 from <64) so a release event doesn't double-fire. CCs and notes are both channel-agnostic — 8mu's per-bank channel setting doesn't matter to us.
- **Randomize action (note 72 / C5):** Re-rolls all 8 step pitches and velocities by calling `randomize_pattern()` in `midi_host.cpp`, using the same pitch range (C2–B4) and velocity floor (100–255) as the constructor's boot randomisation. A dedicated Core-1 xorshift32 PRNG lives in `midi_host.cpp`, seeded lazily from `time_us_32()` on first use — separate from the audio ISR's PRNG on Core 0 so a button press doesn't perturb the white-noise stream. Single-byte writes are atomic, so a randomize-during-playback might briefly mix new and old values mid-pattern, which is the desired "instant scramble" sound.
- **No 8mu pickup:** Unlike the panel knobs, 8mu faders write directly on every CC RX without pickup logic. 8mu only sends on change, so an unmoved fader never overwrites a parameter — the "jump on first move after mode toggle" behaviour is desirable here (you intentionally moved that fader; writing its value is what you want).
