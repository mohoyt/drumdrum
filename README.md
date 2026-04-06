# drumdrum

A DFAM-style 8-step sequencer for the [Music Thing Modular Workshop System Computer](https://www.musicthing.co.uk/Workshop-Computer/).

drumdrum gives you a dual-VCO pitch sequencer with per-step velocity, white noise, step triggers, and end-of-cycle triggers — the core building blocks of a DFAM-style percussion voice, all from a single program card. Sequence data is randomised on every reset, so you can roll the dice on a new pattern any time.

## Controls

### Switch Positions

| Position | Main Knob | X Knob | Y Knob |
|----------|-----------|--------|--------|
| **Up** (play) | Tempo | Sequence length (2–8 steps) | VCO 2 pitch offset (±24 semitones) |
| **Middle** (edit) | Tempo | Step pitch (0–127 MIDI note) | Step velocity (0–255) |
| **Down** (momentary) | Short press: advance edit cursor. Long press (hold ≥500ms): toggle play/pause | | |

All knobs use pickup/catchup behaviour when switching modes — the knob must pass through the stored value before it takes effect, preventing jumps.

### Play Mode (switch UP)

The sequencer runs, cycling through the active steps. LEDs show the current playback step.

- **Main** controls the internal clock tempo (overridden when an external clock is patched to Pulse In 1).
- **X** sets how many steps are active (2–8). The change takes effect immediately.
- **Y** transposes VCO 2's pitch relative to VCO 1, in semitones. At noon the two VCOs play in unison; turn CW for higher, CCW for lower. Use this to set intervals (fifths, octaves) or detune for thickness.

### Edit Mode (switch MIDDLE)

Playback continues uninterrupted. The LEDs switch to showing the edit cursor position instead of the playback position.

- **X** sets the pitch for the step at the edit cursor.
- **Y** sets the velocity for that step.
- **Short press down** advances the edit cursor to the next step (wraps at sequence length).
- **Long press down** (hold ≥500ms) toggles play/pause. When paused, advancing the cursor fires a preview trigger on Pulse Out 1 so you can hear each step through your patch.

### LED Encoding

The 6 LEDs (2 columns, 3 rows) encode steps 1–8:

```
Step 1:  *  .     Step 5:  *  *
         .  .              *  *
         .  .              *  .

Step 2:  *  *     Step 6:  *  *
         .  .              *  *
         .  .              *  *

Step 3:  *  *     Step 7:  .  *
         *  .              *  *
         .  .              *  *

Step 4:  *  *     Step 8:  .  .
         *  *              *  *
         .  .              *  *
```

This encoding is used for playback position, edit cursor, and sequence length preview.

## Jacks

| Jack | Function |
|------|----------|
| **CV Out 1** | VCO 1 pitch (1V/oct, EEPROM-calibrated, quantised to semitones) |
| **CV Out 2** | Velocity CV (for controlling envelope decay via Slope or similar) |
| **Audio Out 1** | White noise (continuous, always running) |
| **Audio Out 2** | VCO 2 pitch CV (same sequence as VCO 1, offset by Y knob in play mode) |
| **Pulse Out 1** | Step trigger (~2ms pulse each step; preview trigger when paused in edit mode) |
| **Pulse Out 2** | End-of-cycle trigger (fires when the sequence wraps from last step back to first) |
| **Pulse In 1** | External clock input (overrides internal tempo; one step per rising edge) |
| **Pulse In 2** | Reset (returns sequence to step 1 on rising edge) |
| **CV In 1** | Decay CV mod (summed with stored velocity for the current step) |
| **CV In 2** | Global pitch transpose (±24 semitones, affects both CV Out 1 and Audio Out 2) |

## Patch Examples

These patches are written for the Workshop System's analogue section. The Computer's outputs connect to the SineSquare oscillators, Slopes, Ring Mod, Humpback filters, and Mix.

A quick reminder of what's available: two SineSquare VCOs (each with a pitch/FM input, sine output, and square output), two Slopes (signal input, CV input for rate modulation, and output; 3-position switch for Loop/off/Blip), a Ring Mod (audio input, modulation input, output — works as a VCA when one input receives an envelope), two Humpback filters (audio in, FM in, CV in for cutoff modulation, Res knob, LP out, BP/HP out), and the Mix (four channel inputs with pan on 1–2, L/R outputs, headphone out).

### 1. Classic DFAM Percussion Voice

The essential drum patch: a pitched oscillator shaped by an envelope, with per-step velocity controlling the decay time — just like the real DFAM.

```
CV Out 1          --> SineSquare 1 pitch input
CV Out 2          --> Slope 1 CV input (velocity controls decay time)
Pulse Out 1       --> Slope 1 signal input (step trigger)
SineSquare 1 sine --> Ring Mod audio input
Slope 1 output    --> Ring Mod modulation input (acts as VCA)
Ring Mod output   --> Mix channel 1
Audio Out 1       --> Mix channel 2 (noise layer)
```

Set the SineSquare 1 FM attenuverter knob to around 2 o'clock for 1V/oct tracking. The Slope fires on each step trigger and its envelope output controls the Ring Mod as a VCA. The velocity CV modulates the Slope's rate via its CV input — high-velocity steps get a longer decay (accented hits), low-velocity steps get a shorter, tighter sound. Mix in the white noise on a second channel for snare-like transients, or turn it down for pure pitched drums.

Switch to edit mode (MIDDLE) to program pitches per step with X and velocities with Y. Reset the Computer for a fresh random pattern.

### 2. Dual-VCO Sequence Through Filter

Both pitch outputs drive the two SineSquare oscillators with an interval between them. Velocity modulates the filter cutoff for per-step timbral variation.

```
CV Out 1          --> SineSquare 1 pitch input
Audio Out 2       --> SineSquare 2 pitch input
SineSquare 1 sine --> Humpback 1 audio input
CV Out 2          --> Humpback 1 CV input (velocity controls cutoff)
Pulse Out 1       --> Slope 1 signal input (trigger)
Slope 1 output    --> Ring Mod modulation input (VCA envelope)
Humpback 1 LP out --> Ring Mod audio input
Ring Mod output   --> Mix channel 1
SineSquare 2 sine --> Mix channel 2
```

Set Y knob (play mode) to noon for unison, slightly off for detuned chorus, or to +7 for a fifth. SineSquare 1 goes through the Humpback filter — with the velocity CV driving the cutoff, high-velocity steps sound brighter and low-velocity steps stay dark. The Ring Mod acts as a VCA controlled by the Slope envelope. SineSquare 2 goes straight to the Mix as a raw drone layer. Set the Humpback resonance high for an acid flavour.

Note: Audio Out 2 outputs pitch CV (not audio) — it approximates 1V/oct on the audio DAC. Set SineSquare 2's FM attenuverter to taste and tune by ear.

### 3. Noise Percussion with Slope Shaping

Use the white noise through a filter and envelope for hi-hat or snare textures. Velocity controls both the decay length and the filter brightness.

```
Audio Out 1       --> Humpback 1 audio input
CV Out 2          --> Humpback 1 CV input (velocity opens/closes filter)
Pulse Out 1       --> Slope 1 signal input (trigger)
CV Out 2          --> Slope 1 CV input (velocity controls decay — mult or stack)
Slope 1 output    --> Ring Mod modulation input
Humpback 1 BP out --> Ring Mod audio input
Ring Mod output   --> Mix channel 1
```

The noise runs through the Humpback filter (bandpass output with resonance up for metallic hats, or lowpass for snare body), then through the Ring Mod as a VCA. Velocity does double duty here: it opens the filter cutoff and lengthens the Slope decay on high-velocity steps for bright, ringy hits, while low-velocity steps stay short and dull. You'll need a mult or stacked cable to send CV Out 2 to both the Humpback CV and Slope CV inputs — or choose one destination and experiment.

### 4. Externally Clocked with 4 Voltages Transpose

Use the 4 Voltages module as a live performance keyboard to transpose the running sequence.

```
4 Voltages output --> CV In 2 (global transpose, ±24 semitones)
CV Out 1          --> SineSquare 1 pitch input
CV Out 2          --> Slope 1 CV input (velocity controls decay)
Pulse Out 1       --> Slope 1 signal input (trigger)
SineSquare 1 sine --> Ring Mod audio input
Slope 1 output    --> Ring Mod modulation input
Ring Mod output   --> Mix channel 1
Pulse Out 2       --> Slope 2 signal input (end-of-cycle trigger)
```

The sequence runs on the internal clock (Main knob). Pressing buttons on 4 Voltages shifts the whole pattern up or down in pitch via CV In 2. Velocity controls the Slope decay per step, so accented steps ring out while ghost notes stay tight. Pulse Out 2 fires once per cycle — patch it to Slope 2 to create a slow envelope that marks the downbeat, useful for modulating filter cutoff or another parameter at the phrase level. Shorten the sequence to 3 or 4 steps with X knob for tighter, faster loops that respond well to live transposition.

## Randomisation

Step pitches and velocities are randomised on every reset. If you want a new pattern, just press the reset button on the Workshop Computer. The randomisation covers a 3-octave pitch range (C2–B4) with velocities biased toward the upper half so all steps are audible out of the box.

## Building

Requires the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk).

```bash
mkdir build && cd build
cmake ..
make
```

Flash the resulting `drumdrum.uf2` to the Workshop Computer by holding BOOT while pressing RESET, then dragging the file to the mounted USB drive.

## Technical Details

- Single-file implementation (`main.cpp`), ~450 lines
- All DSP and sequencer logic runs in `ProcessSample()` at 48kHz in interrupt context
- Pure integer arithmetic throughout — no float, no division
- White noise via xorshift32 PRNG, seeded from hardware timer on each boot
- CV Out 1 uses EEPROM-calibrated `CVOutMIDINote()` for accurate 1V/oct tracking
- Audio Out 2 approximates 1V/oct on the 12-bit audio DAC (~28.4 DAC units/semitone, uncalibrated)
- System clock set to 144MHz to reduce ADC tonal artifacts
- All code copied to RAM (`copy_to_ram`) to eliminate flash cache jitter

## License

MIT
