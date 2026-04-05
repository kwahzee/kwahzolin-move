# kwahzolin

Chaotic rungler synthesizer for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move.

A dual-oscillator instrument built around an 8-bit shift register (rungler) that feeds back into itself, creating evolving, self-similar patterns that range from melodic and rhythmic at low chaos to unpredictable and noisy at high chaos.

## Signal Chain

```
Osc1 (triangle, freq ← rungler CV × Chaos)   → tanh
Osc2 (triangle, free-running rungler clock)   → tanh
Mix Osc1 + Osc2                               → tanh
Ring mod: Osc1 × Osc2 (Ring Mod knob)         → tanh
Drive saturation                              → tanh
State variable lowpass filter (high resonance)
Sequencer gate (16 steps, MIDI-clock synced)
Output                                        → tanh
```

The rungler shift register is clocked by Osc2 zero-crossings. Its accumulated value modulates Osc1's frequency and the filter cutoff. At high resonance + high drive the filter self-oscillates and screams.

## Controls

### 8 Knobs

| # | Knob        | Description                                        |
|---|-------------|----------------------------------------------------|
| 1 | Osc1 Rate   | Osc1 frequency (0.5 Hz–5 kHz, exponential)         |
| 2 | Osc2 Rate   | Osc2 frequency / rungler clock rate                |
| 3 | Chaos       | Rungler CV depth into Osc1 frequency               |
| 4 | Cutoff      | SVF base cutoff (20 Hz–20 kHz, exponential)        |
| 5 | Resonance   | SVF Q — high values cause self-oscillation         |
| 6 | Drive       | Pre-filter saturation (1× to 20×)                  |
| 7 | Rungler Mod | How much rungler CV opens the filter               |
| 8 | Ring Mod    | Osc1 × Osc2 mix into signal                       |

### 32 Pads — Loop Control

Each pad arms a rungler loop of N beats (pad 1 = 1 beat, pad 32 = 32 beats).

- **Press an inactive pad**: snapshot the current shift register state and begin looping it at that beat length, locked to MIDI clock tempo.
- **Press the active pad again**: release the loop, return to free-running chaos.
- **Press a different pad**: re-record from the current state at the new beat length.

Loop length follows MIDI clock tempo in real time.

### 16 Step Buttons — Gate Sequencer

Step buttons toggle individual steps in a 16-step gate sequencer.

- Lit step = audio passes through
- Dark step = audio muted
- Gate advances every beat (24 MIDI clock ticks)
- With no MIDI clock: gate advances every 4 Osc2 cycles

## Display

```
┌────────────────────────────────┐
│ KWAHZOLIN         (or knob name)│
│ FREE / LOOP 4b   (or knob %)   │
│ 120 BPM                        │
│ ▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░ │  ← 16 step blocks
└────────────────────────────────┘
```

Knob name and value are shown for ~1.5 seconds after a knob is moved.

## Building

Requires Docker Desktop (macOS/Windows) or Docker Engine (Linux).

```bash
./scripts/build.sh
```

Output: `dist/kwahzolin-module.tar.gz`

### Without Docker (native ARM64 or with cross-compiler)

```bash
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

## Installing

```bash
./scripts/install.sh
# or
MOVE_HOST=192.168.x.y ./scripts/install.sh
```

The module installs to `/data/UserData/schwung/modules/sound_generators/kwahzolin/` on the device.

## Sound Tips

- **Low chaos, slow Osc2**: melodic, almost sequencer-like — the rungler produces a small repeating set of values
- **High chaos, fast Osc2**: rapid unpredictable evolution — use pads to lock a moment into a loop
- **Loop + sequencer gate**: the repeating rungler pattern becomes a rhythmic groove with gate-driven articulation
- **High resonance + high drive**: the filter screams; use Cutoff and Rungler Mod to shape the aggression
- **Ring Mod above 50%**: metallic, clangorous overtones; combine with high resonance for industrial textures
