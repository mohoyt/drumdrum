/*
 * drumdrum — DFAM-Style Step Sequencer
 * =====================================
 * A program card for the Music Thing Modular Workshop Computer that emulates
 * the core sequencer and sound-generation behaviour of the Moog DFAM.
 *
 * The sequencer drives two VCOs (via CV Out 1 and Audio Out 2), outputs a
 * velocity CV for envelope/decay control, generates continuous white noise,
 * and provides step triggers and end-of-cycle triggers for external patching.
 *
 *
 * I/O Assignment:
 * ───────────────
 *   CV Out 1    — VCO 1 pitch (1V/oct, calibrated, quantised to semitones)
 *   CV Out 2    — Velocity CV (for EG decay when patched to Slope)
 *   Audio Out 1 — White noise (continuous, audio-rate xorshift32 PRNG)
 *   Audio Out 2 — VCO 2 pitch (same sequence, offset by Y knob in play mode)
 *   Pulse Out 1 — Step trigger (fires each step; also preview when paused)
 *   Pulse Out 2 — End-of-cycle trigger (fires when sequence wraps to step 1)
 *   Pulse In 1  — External clock (overrides internal tempo when patched)
 *   Pulse In 2  — Reset (returns sequence to step 1 on rising edge)
 *   CV In 1     — Decay CV mod (summed with stored velocity for current step)
 *   CV In 2     — Global pitch transpose (±24 semitones across both VCOs)
 *
 *
 * Modes (Z switch):
 * ─────────────────
 *   UP (play)   — Main=tempo, X=length(2-8), Y=VCO2 offset
 *   MIDDLE (edit)— Main=tempo, X=step pitch(pickup), Y=step velocity(pickup)
 *   DOWN (momentary) — short press: advance edit cursor
 *                       long press (≥500ms): toggle play/pause
 *
 *
 * LED Encoding (steps 1–8 shown on the 6-LED 2×3 grid):
 * ──────────────────────────────────────────────────────
 *   Step 1: top-left only
 *   Step 2: top-left + top-right
 *   Step 3: top row + middle-left
 *   Step 4: top + middle rows
 *   Step 5: top + middle + bottom-left
 *   Step 6: all 6 LEDs
 *   Step 7: all except top-left
 *   Step 8: middle + bottom rows (4 LEDs)
 */

#include "ComputerCard.h"
#include "hardware/clocks.h"


class DFAMSequencer : public ComputerCard
{
    // ──────────────────────────────────────────────
    // Per-step sequence data
    // ──────────────────────────────────────────────
    struct Step {
        uint8_t pitch;    // 0–127 (MIDI note number)
        uint8_t velocity; // 0–255 (mapped to CV Out 2 voltage)
    };
    Step steps[8];

    // ──────────────────────────────────────────────
    // Sequencer state
    // ──────────────────────────────────────────────
    int currentStep = 0;    // playback position (0-based, 0–7)
    int seqLength   = 8;    // number of active steps (2–8)
    bool playing    = true;

    // ──────────────────────────────────────────────
    // Internal clock timing
    // At 48kHz, ticksPerStep sets the step duration.
    // Quadratic mapping from Main knob gives a musical
    // tempo feel: CW = fast (~30 steps/s), CCW = slow (~2s/step).
    // ──────────────────────────────────────────────
    uint32_t tickCounter  = 0;
    uint32_t ticksPerStep = 48000;

    // ──────────────────────────────────────────────
    // Trigger pulse timing
    // Each trigger stays high for TRIGGER_LEN samples (~2ms),
    // long enough for any envelope generator to detect reliably.
    // ──────────────────────────────────────────────
    static constexpr int TRIGGER_LEN = 96; // ~2ms at 48kHz
    int trigCounter = TRIGGER_LEN; // initialised high so step 1 triggers at startup
    int eocCounter  = 0;

    // ──────────────────────────────────────────────
    // Edit mode state
    // ──────────────────────────────────────────────
    int editStep     = 0;
    bool xPickedUp   = false;
    bool yPickedUp   = false;

    // Pickup threshold: the knob must come within this many units of the
    // stored value before it "catches" and begins tracking. This prevents
    // value jumps when switching modes or between steps (MicroFreak-style).
    static constexpr int PICKUP_THRESH = 3;
    static constexpr int PICKUP_THRESH_LENGTH = 1; // tighter for the 2–8 range

    // ──────────────────────────────────────────────
    // Play mode pickup state
    // Same pickup/catchup behaviour applies when switching back to play mode,
    // so the X (length) and Y (VCO2 offset) knobs don't jump to a new value
    // if they were left in a different position during edit mode.
    // ──────────────────────────────────────────────
    bool lengthPickedUp = false;
    bool offsetPickedUp = false;

    // ──────────────────────────────────────────────
    // VCO 2 pitch offset (semitones, set by Y knob in play mode)
    // Stored as a member so it persists when switching to edit mode.
    // ──────────────────────────────────────────────
    int32_t vco2Offset = 0;

    // ──────────────────────────────────────────────
    // Switch DOWN (momentary) press detection
    // The physical switch is (ON)-OFF-ON: UP latches, MIDDLE is centre,
    // DOWN is momentary and springs back to MIDDLE.
    // We time how long DOWN is held to distinguish short/long presses.
    // ──────────────────────────────────────────────
    static constexpr uint32_t LONG_PRESS_SAMPLES = 24000; // 500ms at 48kHz
    bool     switchDownActive = false;
    uint32_t switchDownCount  = 0;
    bool     longPressHandled = false;

    // ──────────────────────────────────────────────
    // Previous switch position (for detecting mode transitions)
    // ──────────────────────────────────────────────
    Switch prevMode = Switch::Up;

    // ──────────────────────────────────────────────
    // White noise PRNG state (xorshift32)
    // ──────────────────────────────────────────────
    uint32_t rngState = 0xDEADBEEF;

    // ──────────────────────────────────────────────
    // LED encoding table
    // Each entry is a 6-bit mask: bit N = LED N on.
    // Index 0 = step 1, index 7 = step 8.
    //
    // LED layout:
    //   | 0  1 |  (top)
    //   | 2  3 |  (middle)
    //   | 4  5 |  (bottom)
    // ──────────────────────────────────────────────
    static constexpr uint8_t ledPattern[8] = {
        0b000001, // step 1: top-left
        0b000011, // step 2: top-left + top-right
        0b000111, // step 3: + middle-left
        0b001111, // step 4: + middle-right
        0b011111, // step 5: + bottom-left
        0b111111, // step 6: all ON
        0b111110, // step 7: all except top-left
        0b111100, // step 8: middle + bottom rows
    };

public:
    // Advance the PRNG by one step and return the new state.
    uint32_t __not_in_flash_func(xorshift)()
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return rngState;
    }

    DFAMSequencer()
    {
        // Seed the PRNG from the hardware microsecond timer so that each
        // reset produces a different sequence. The exact timer value at
        // this point depends on boot timing and is effectively random.
        rngState = time_us_32();
        if (rngState == 0) rngState = 0xDEADBEEF; // xorshift must never be zero

        // Randomise step values: pitch within a musically useful 3-octave
        // range (C2–B4, MIDI 36–71), velocity biased toward the upper half
        // so steps are audible by default.
        for (int i = 0; i < 8; i++) {
            steps[i].pitch    = 36 + (xorshift() % 36);  // C2–B4
            steps[i].velocity = 100 + (xorshift() % 156); // 100–255
        }
    }

    // ProcessSample runs once per audio sample at 48kHz in interrupt context.
    // It must complete within ~20μs. All arithmetic is integer to stay fast.
    virtual void ProcessSample() override
    {
        // ════════════════════════════════════════════
        // WHITE NOISE — always running on Audio Out 1,
        // regardless of mode or play/pause state.
        // xorshift32 produces a full-spectrum noise signal.
        // ════════════════════════════════════════════
        // Extract 12 bits and centre around zero for the signed audio range
        AudioOut1(static_cast<int16_t>((xorshift() >> 20) & 0xFFF) - 2048);


        // ════════════════════════════════════════════
        // READ SWITCH — determines current mode
        // ════════════════════════════════════════════
        Switch sw = SwitchVal();

        // When entering edit mode, reset edit-knob pickup so X/Y don't
        // immediately overwrite the step's stored values.
        if (sw == Switch::Middle && prevMode != Switch::Middle) {
            xPickedUp = false;
            yPickedUp = false;
        }

        // When entering play mode, reset play-knob pickup so X (length)
        // and Y (VCO2 offset) don't jump from their edit-mode positions.
        if (sw == Switch::Up && prevMode != Switch::Up) {
            lengthPickedUp = false;
            offsetPickedUp = false;
        }


        // ════════════════════════════════════════════
        // SWITCH DOWN — momentary press handling
        // Short press (<500ms): advance edit cursor to next step.
        // Long press (≥500ms): toggle play/pause.
        // ════════════════════════════════════════════
        if (sw == Switch::Down) {
            if (!switchDownActive) {
                switchDownActive = true;
                switchDownCount  = 0;
                longPressHandled = false;
            }
            switchDownCount++;

            if (switchDownCount >= LONG_PRESS_SAMPLES && !longPressHandled) {
                // Long press detected: toggle play/pause
                longPressHandled = true;
                if (playing) {
                    // Pause the sequencer — step position is preserved
                    playing = false;
                } else {
                    // Resume playback from the step after the last one played.
                    // This avoids re-triggering a step the user already heard.
                    playing = true;
                    currentStep = (currentStep + 1) % seqLength;
                    tickCounter = 0;
                    trigCounter = TRIGGER_LEN;
                    if (currentStep == 0) {
                        eocCounter = TRIGGER_LEN;
                    }
                }
            }
        } else if (switchDownActive) {
            // Switch just released back to MIDDLE
            switchDownActive = false;

            if (!longPressHandled) {
                // Short press: advance edit cursor, wrapping at sequence length
                editStep = (editStep + 1) % seqLength;
                xPickedUp = false;
                yPickedUp = false;

                // When paused, fire a preview trigger so the user can hear
                // the step they just navigated to. This does NOT fire during
                // normal playback to avoid double-triggering.
                if (!playing) {
                    trigCounter = TRIGGER_LEN;
                }
            }
        }


        // ════════════════════════════════════════════
        // MODE-DEPENDENT KNOB HANDLING
        // ════════════════════════════════════════════
        if (sw == Switch::Up) {
            // --- Play mode ---
            // Both knobs use pickup behaviour so switching from edit mode
            // doesn't cause the length or VCO2 offset to jump.

            // X knob → sequence length (2–8 steps)
            int32_t knobLength = 2 + ((KnobVal(Knob::X) * 7) >> 12);
            if (!lengthPickedUp) {
                int32_t d = knobLength - seqLength;
                if (d < 0) d = -d;
                if (d <= PICKUP_THRESH_LENGTH) lengthPickedUp = true;
            }
            if (lengthPickedUp) {
                seqLength = knobLength;
            }

            // Y knob → VCO 2 pitch offset relative to VCO 1 (±24 semitones)
            // Knob at noon ≈ unison; CW = higher, CCW = lower
            int32_t knobOffset = ((KnobVal(Knob::Y) * 49) >> 12) - 24;
            if (!offsetPickedUp) {
                int32_t d = knobOffset - vco2Offset;
                if (d < 0) d = -d;
                if (d <= PICKUP_THRESH) offsetPickedUp = true;
            }
            if (offsetPickedUp) {
                vco2Offset = knobOffset;
            }

        } else if (sw == Switch::Middle) {
            // --- Edit mode ---
            // Knobs use "pickup" behaviour: the knob must pass through the
            // stored value before it begins tracking. This prevents jarring
            // value jumps when the knob position doesn't match the step's
            // stored value.

            // X knob → pitch for the current edit step (0–127 MIDI note)
            int32_t knobPitch = (KnobVal(Knob::X) * 127) >> 12;
            if (!xPickedUp) {
                int32_t d = knobPitch - static_cast<int32_t>(steps[editStep].pitch);
                if (d < 0) d = -d;
                if (d <= PICKUP_THRESH) xPickedUp = true;
            }
            if (xPickedUp) {
                steps[editStep].pitch = static_cast<uint8_t>(knobPitch);
            }

            // Y knob → velocity for the current edit step (0–255)
            int32_t knobVel = (KnobVal(Knob::Y) * 255) >> 12;
            if (!yPickedUp) {
                int32_t d = knobVel - static_cast<int32_t>(steps[editStep].velocity);
                if (d < 0) d = -d;
                if (d <= PICKUP_THRESH) yPickedUp = true;
            }
            if (yPickedUp) {
                steps[editStep].velocity = static_cast<uint8_t>(knobVel);
            }
        }


        // ════════════════════════════════════════════
        // RESET INPUT
        // Rising edge on Pulse In 2 returns the sequence to step 1.
        // ════════════════════════════════════════════
        if (PulseIn2RisingEdge()) {
            currentStep = 0;
            tickCounter = 0;
            trigCounter = TRIGGER_LEN;
        }


        // ════════════════════════════════════════════
        // SEQUENCER ADVANCE
        // External clock (Pulse In 1) overrides the internal tempo when
        // patched. The internal clock keeps running in the background so
        // that removing the patch cable resumes at the current tempo.
        // ════════════════════════════════════════════
        bool useExtClock = Connected(Input::Pulse1);

        if (playing) {
            bool advance = false;

            if (useExtClock) {
                // External clock: each rising edge advances one step
                if (PulseIn1RisingEdge()) {
                    advance = true;
                }
            } else {
                // Internal clock: Main knob sets tempo with a quadratic curve
                // for a musical feel. CW = fast (~30 steps/s), CCW = slow (~2s).
                int32_t knob = KnobVal(Knob::Main);
                int32_t inv  = 4095 - knob;
                ticksPerStep = 1600 + ((inv * inv * 6) >> 10);

                tickCounter++;
                if (tickCounter >= ticksPerStep) {
                    tickCounter = 0;
                    advance = true;
                }
            }

            if (advance) {
                currentStep = (currentStep + 1) % seqLength;
                trigCounter = TRIGGER_LEN;
                // Fire end-of-cycle when wrapping from last step back to first
                if (currentStep == 0) {
                    eocCounter = TRIGGER_LEN;
                }
            }
        } else {
            // While paused, keep updating the tempo calculation so that
            // unplugging an external clock cable resumes smoothly.
            if (!useExtClock) {
                int32_t knob = KnobVal(Knob::Main);
                int32_t inv  = 4095 - knob;
                ticksPerStep = 1600 + ((inv * inv * 6) >> 10);
            }
        }

        // Clamp positions if sequence length was shortened
        if (currentStep >= seqLength) currentStep = 0;
        if (editStep >= seqLength)    editStep    = 0;


        // ════════════════════════════════════════════
        // CV AND AUDIO OUTPUTS
        // ════════════════════════════════════════════

        // When paused in edit mode, output the edit step's values so the
        // user can hear what they are editing via the preview trigger.
        // Otherwise, output the current playback step.
        int outputStep = currentStep;
        if (!playing && sw == Switch::Middle) {
            outputStep = editStep;
        }
        Step &active = steps[outputStep];

        // CV In 2 → global pitch transpose (±24 semitones)
        // CVIn2 returns -2048..+2047; scale to ±24 with integer math
        int32_t transpose = (CVIn2() * 3) >> 8;

        // --- CV Out 1: VCO 1 pitch (calibrated 1V/oct) ---
        // Uses the EEPROM-calibrated CVOutMIDINote for accurate tracking
        int32_t pitch1 = static_cast<int32_t>(active.pitch) + transpose;
        if (pitch1 < 0)   pitch1 = 0;
        if (pitch1 > 127) pitch1 = 127;
        CVOut1MIDINote(static_cast<uint8_t>(pitch1));

        // --- CV Out 2: Velocity + Decay CV mod ---
        // Velocity (0–255) is scaled to fill the positive CV range.
        // CV In 1 is summed in to allow external modulation of the
        // decay/envelope amount.
        int32_t velCV = static_cast<int32_t>(active.velocity) * 8 + CVIn1();
        if (velCV > 2047)  velCV = 2047;
        if (velCV < -2048) velCV = -2048;
        CVOut2(static_cast<int16_t>(velCV));

        // --- Audio Out 2: VCO 2 pitch CV ---
        // Approximates 1V/oct on the 12-bit audio DAC.
        // Audio DAC range -2048..+2047 ≈ ±6V, so 1 semitone ≈ 28.4 units.
        // Formula: (midiNote - 60) * 1820 >> 6 gives ~28.44 units/semitone.
        // This output is uncalibrated — tune VCO 2 by ear or with a tuner.
        int32_t pitch2 = static_cast<int32_t>(active.pitch) + transpose + vco2Offset;
        int32_t audioPitch = ((pitch2 - 60) * 1820) >> 6;
        if (audioPitch > 2047)  audioPitch = 2047;
        if (audioPitch < -2048) audioPitch = -2048;
        AudioOut2(static_cast<int16_t>(audioPitch));


        // ════════════════════════════════════════════
        // TRIGGER OUTPUTS
        // Each trigger pulse counts down from TRIGGER_LEN (~2ms).
        // ════════════════════════════════════════════
        PulseOut1(trigCounter > 0);
        if (trigCounter > 0) trigCounter--;

        PulseOut2(eocCounter > 0);
        if (eocCounter > 0) eocCounter--;


        // ════════════════════════════════════════════
        // LED DISPLAY
        // In edit mode: shows the edit cursor position.
        // In play mode: shows the current playback step.
        // Both use the same step-encoding scheme (see table above).
        // ════════════════════════════════════════════
        int displayStep = (sw == Switch::Middle) ? editStep : currentStep;
        uint8_t pattern = ledPattern[displayStep];
        for (int i = 0; i < 6; i++) {
            LedOn(i, (pattern >> i) & 1);
        }

        prevMode = sw;
    }
};


int main()
{
    // 144MHz reduces ADC tonal artifacts (recommended by Workshop Computer
    // AI Directive). Must be set before the ComputerCard constructor, which
    // configures peripheral clocks based on the current system frequency.
    set_sys_clock_khz(144000, true);

    DFAMSequencer seq;
    seq.EnableNormalisationProbe();
    seq.Run();
}
