# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**drumdrum** is a DFAM-style 8-step sequencer program card for the Music Thing Modular Workshop Computer. It runs on an RP2040 (Cortex M0+) using the ComputerCard header-only C++ library at a fixed 48kHz sample rate.

The WORKSHOP_COMPUTER_AI_DIRECTIVE.md file in this repo is the authoritative reference for platform constraints, API details, and coding standards. Read it before making changes.

## Build

Requires the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) with `PICO_SDK_PATH` set.

```bash
mkdir build && cd build
cmake ..
make
```

Output: `build/drumdrum.uf2` — flash to Workshop Computer by holding BOOT and pressing RESET, then drag the UF2 onto the USB drive.

## Architecture

Single-file design (`main.cpp`). `DFAMSequencer` subclasses `ComputerCard` and overrides `ProcessSample()`, which runs in an ISR at 48kHz and must complete within ~20μs.

**Key constraints:**
- All arithmetic is `int32_t` — no float, no division. Multiply + shift only.
- `ProcessSample` runs in interrupt context. No allocations, no blocking, no prints.
- System clock is 144MHz (set before ComputerCard construction to reduce ADC artifacts).
- `copy_to_ram` binary type eliminates flash cache jitter.

## I/O Map

| Jack | Function |
|------|----------|
| CV Out 1 | VCO 1 pitch (calibrated 1V/oct via `CVOut1MIDINote`) |
| CV Out 2 | Velocity CV (summed with CV In 1 for decay modulation) |
| Audio Out 1 | White noise (xorshift32 PRNG, always running) |
| Audio Out 2 | VCO 2 pitch (uncalibrated, ~28.4 DAC units/semitone) |
| Pulse Out 1 | Step trigger (~2ms pulse) / edit preview trigger when paused |
| Pulse Out 2 | End-of-cycle trigger |
| Pulse In 1 | External clock (overrides internal tempo) |
| Pulse In 2 | Reset to step 1 |
| CV In 1 | Decay CV mod |
| CV In 2 | Global pitch transpose (±24 semitones) |

## Mode Behaviour

- **Switch UP (play):** Main=tempo, X=sequence length (2–8), Y=VCO2 pitch offset (±24 semitones). LEDs show playback step.
- **Switch MIDDLE (edit):** Main=tempo, X=step pitch (pickup), Y=step velocity (pickup). LEDs show edit cursor. Playback continues independently.
- **Switch DOWN (momentary):** Short press (<500ms) advances edit cursor. Long press (≥500ms) toggles play/pause. Preview trigger fires when cursor moves while paused.

## Key Design Decisions

- **Pickup/catchup knobs:** In edit mode, X and Y knobs don't affect the stored value until the knob passes within ±3 units of it. This prevents jumps when the knob doesn't match the stored step value.
- **Tempo curve:** Quadratic mapping `1600 + (inv² × 6) >> 10` gives ~30 steps/s to ~2s/step. Musical feel, no float.
- **Audio Out 2 pitch scaling:** `(note - 60) * 1820 >> 6` approximates 1V/oct on the 12-bit audio DAC. Not EEPROM-calibrated — tune VCO 2 by ear.
- **LED encoding steps 1–8:** Fill LEDs 0–5 sequentially for steps 1–6; steps 7–8 drop the top LEDs. See `ledPattern[]` array.
- **External clock:** `Connected(Input::Pulse1)` via normalisation probe detects patching. Internal tempo keeps updating in background so unplugging resumes smoothly.
