# kwahzolin

*a benjolin-inspired synthesizer module for the ableton move unofficial firmware [schwung](https://github.com/charlesvestal/schwung)*

---

```
        ~~~  the rungler  ~~~

    osc2 ──▶ [clock] ──▶ [ 8 bit shift register ]
                              │   │   │   │
                         bit7  bit5  bit3  bit0
                              └───┬───┘
                               [sum] ──▶ rungler cv ──▶ osc1 freq
                                                    └──▶ filter cutoff

    osc2 clocks the shift register on every
    positive zero crossing. osc1's sign becomes
    the new bit. the register members the fergets.
```

2 osc, triangle-shaped, circling each other. osc2 is the clock — it ticks the rungler on every upswing, pushing a new bit into the shift register from osc1's sign. the accumulated register value spills back as voltage into osc1's frequency and into the filter's mouth. this is the rungler, rob hordijk's circuit.

---

```
    ·  ·  ·  signal path  ·  ·  ·

    [osc1]──tanh──┐
                  ├──mix──tanh──[ring]──tanh──[filter]──[gate]──▶ out
    [osc2]──tanh──┘                              ▲
                                         rungler cv
                                    (jumping, stepping,
                                      pinging the poles)
```

---

## filter

two integrators in cascade, feedback saturated through hyperbolic tangent so it cannot blow apart. at low resonance it is warm and slightly furry. at high resonance it begins to ping n pop like a plucked string when the rungler steps the cutoff underneath it.

```
         input
           │
      [tanh × drive]           ← filter drive knob furs the signal
           │
      ┌────┴────┐
      │  pole 1 │◀──[tanh × resonance]◀──┐
      │  low1  ─┼──────────────────────────┘
      └────┬────┘   (feedback from pole 1 output,
           │         bounded, cannot explode)
      ┌────┴────┐
      │  pole 2 │
      │  low2   │
      └────┬────┘
           │
      [tanh × 0.8]
           │
          out

    when filter chaos is high and resonance is high,
    each rungler step jerks the cutoff to a new place
    and the feedback loop rings there drip drip drip 
    like water finding new paths through stone
```

---

## knobs

```
    ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐
    │  1  │ │  2  │ │  3  │ │  4  │ │  5  │ │  6  │ │  7  │ │  8  │
    └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘
       │       │       │       │       │       │       │       │
      osc1    osc2    osc     filt    filt    filt    filt    ring
      freq    freq   chaos   cutoff  reson    chaos   drive    mod
```

**osc 1 frequency** — the base pitch of the first triangle.

**osc 2 frequency** — the clock rate of the kwahzolin. slow osc2 means the rungler evolves languidly. fast osc2 and everything seizes up.

**osc chaos** — how deep the rungler digs into osc1's frequency. at zero, osc1 is steady. at maximum, osc1 smears across an octave or more by whatever pattern the shift register has accumulated.

**filter cutoff** — the resting mouth of the filter. the rungler will drag this around if filter chaos is open.

**filter resonance** — the amount of self-feedback. at high values thee filter pings from a rungler step like a small bell struck underwater.

**filter chaos** — how much the rungler's accumulated pattern hurls the filter cutoff around. at high resonance + high filter chaos, every shift register step pings the filter at a different frequency.

**filter drive** — tanh saturation pressed against the filter's mouth.

**ring modulation** — osc1 multiplied by osc2, folded back into the signal.

---

## pads

```
    pad grid, 32 pads:

    [ 1][ 2][ 3][ 4][ 5][ 6][ 7][ 8]
    [ 9][10][11][12][13][14][15][16]
    [17][18][19][20][21][22][23][24]
    [25][26][27][28][29][30][31][32]

    pressing pad N:
      → captures the current shift register state
      → loops it every N osc2 zero-crossings
      → lights all pads 1 through N green

    pressing the last lit pad:
      → releases the loop
      → all pads go dark

    the loop length is measured in osc2 crossings
```

press a pad to catch a moment. the shift register freezes into a pattern and repeats it.

---

## step buttons

```
    16 steps across the bottom of the machine:

    [■][■][■][■][■][□][□][■][■][■][□][□][■][■][□][■]
     1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16

    ■ = gate open, sound passes
    □ = gate closed, silence

    press any step to toggle it.
    they advance with the midi clock (24 ticks per beat).
    without midi clock: every 4 osc2 crossings advances one step.
```

---

## building

docker desktop on mac or windows. docker engine on linux. the cross-compiler lives inside.

```bash
./scripts/build.sh
```

output lands in `dist/kwahzolin-module.tar.gz`

if you already have `aarch64-linux-gnu-gcc` on your machine:

```bash
CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh
```

---

## installing

```bash
./scripts/install.sh
# or specify the move's ip:
MOVE_HOST=192.168.x.y ./scripts/install.sh
```

the module goes to `/data/UserData/schwung/modules/sound_generators/kwahzolin/` on the device.

---

*v0.1.1*
