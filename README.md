# kwahzolin

a benjolin-inspired synthesizer module for the ableton move unofficial firmware [schwung](https://github.com/charlesvestal/schwung)

---

```
    osc2 ──▶ [clock] ──▶ [ 8 bit shift register ]
                              │
                         shift_reg / 255
                              │
                         rungler cv ──▶ osc1 freq
                                   ├──▶ osc2 freq
                                   └──▶ filter cutoff

    osc2 clocks the shift register on every
    positive zero crossing. osc1's sign becomes
    the new bit. the register members then fergets.
```

2 osc, triangle-shaped, circling each other. osc2 is the clock — it ticks the rungler on every upswing, pushing a new bit into the shift register from osc1's sign. the accumulated register value spills back as voltage into both oscillators' frequencies and into the filter's mouth. this is a rungler, rob hordijk's circuit.

---

```
    [osc1] ──▶ pulse ──▶ XOR ──▶ [filter] ──▶ out
    [osc2] ──▶ pulse ──┘               ▲
                                  rungler cv
```

---

## knobs

```
    ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐
    │  1  │ │  2  │ │  3  │ │  4  │ │  5  │ │  6  │ │  7  │ │  8  │
    └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘
       │       │       │       │       │       │       │       │
      osc1    osc2    osc    filter  filter  filter  filter  loop
      freq    freq   chaos   cutoff  reson    lfo    chaos
```

**osc 1 frequency** — the base pitch of the first triangle.

**osc 2 frequency** — the clock rate of the kwahzolin. slow osc2 means the rungler evolves languidly. fast osc2 and everything seizes up.

**osc chaos** — how deep the rungler digs into both oscillators' frequencies. at zero, both run clean at their set rates. at maximum, both smear across an octave or more.

**filter cutoff** — the resting mouth of the filter. the rungler and lfo drag it round.

**filter resonance** — the amount of self-feedback. at high values the filter pings from every rungler step. at maximum it self-oscillates as a sine at the cutoff frequency.

**filter lfo** — how much the slow internal lfo sweeps the cutoff. at zero the lfo has no effect. at maximum it swings up to 2000hz above the base cutoff in a 20-second triangle cycle.

**filter chaos** — the rungler latches a new value on each osc 2 clock tick and holds it until the next.

**loop** — turing machine style register control.

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

v0.1.8
