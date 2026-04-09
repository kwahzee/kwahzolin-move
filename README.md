# kwahzolin

a benjolin-inspired synthesizer module for the ableton move unofficial firmware [schwung](https://github.com/charlesvestal/schwung)

---

```
    osc2 ──▶ [clock] ──▶ [ 8-bit shift register ]
                                    │
                               shift_reg / 255
                                    │
                              rungler cv ──▶ osc1 freq mod
                                        ├──▶ osc2 freq mod
                                        └──▶ filter cutoff mod

    osc2 clocks the register on every positive zero crossing.
    osc1's sign becomes the new bit.
    the accumulated register value spills back as voltage.
    this is a rungler. rob hordijk's circuit.
```

2 oscillators, triangle-shaped, circling each other. osc2 is the clock. every upswing pushes a new bit into the 8-bit shift register from osc1's sign. the register value feeds back into both oscillator frequencies and into the filter's cutoff.

---

```
    [osc1] ──▶ pulse ──▶ XOR ──▶ ring mix ──▶ [svf filter] ──▶ [distortion] ──▶ out
    [osc2] ──▶ pulse ──┘    └──▶ osc1×osc2         ▲
                                               rungler cv + lfo
```

---

## knobs

```
    ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐
    │  1  │ │  2  │ │  3  │ │  4  │ │  5  │ │  6  │ │  7  │ │  8  │
    └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘
       │       │       │       │       │       │       │       │
      osc1    osc2    osc    filter  filter  filter   ring    loop
      freq    freq   chaos   cutoff  reson   chaos    mod
```

**osc 1 frequency** — base pitch of the first triangle oscillator. the rungler and lfo can sweep it.

**osc 2 frequency** — clock rate of the kwahzolin. slow osc2 means the rungler evolves languidly. fast osc2 and everything seizes up.

**osc chaos** — how deep the rungler effects both oscillator frequencies.

**filter cutoff** — resting mouth of the state variable filter. the rungler and lfo drag it around.

**filter resonance** — filter self-feedback amount.

**filter chaos** — scales how far the rungler throws the cutoff on each clock tick.

**ring modulation** — crossfades between the xor pulse signal and true ring mod (osc1 × osc2).

**loop** — turing machine register control. at zero: random. at one: locked. in the middle: mutation.

---

## menu

jog wheel scrolls. jog click selects or toggles edit mode. back button returns to main menu.

### lfo

three lfos. each targets any of the 8 knob parameters at audio-rate resolution. shift + jog switches between lfo 1 / 2 / 3.

| property | range |
|----------|-------|
| shape    | triangle / sine / square / sawtooth / s&h / wander |
| rate     | 0.05 hz – 100 hz |
| amount   | 0.0 – 1.0 |
| target   | any of the 8 knobs |

### distortion

distortion applied after the filter. toggle on/off independently of type and amount.

| type       | character |
|------------|-----------|
| overdrive  | soft tanh saturation, warm and tube-like |
| distortion | hard knee clipping, more aggressive |
| fuzz       | asymmetric hard clip, even harmonics, octave character |

### module

**swap module** — returns to the schwung module picker.  
**unload module** — unloads kwahzolin.

---

## installing

install via the schwung web interface.

---

## building from source

requires `aarch64-linux-gnu-gcc`

```bash
./scripts/build.sh
```

or

```bash
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

output: `dist/kwahzolin-module.tar.gz`

---

v0.2.0
