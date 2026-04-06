/*
 * Kwahzolin — JavaScript UI
 *
 * Handles:
 *   - 8 knobs (CC 71–78, relative delta) → parameters sent to DSP
 *   - Pads 68–99 → loop length control (pad N+1 = N+1 Osc2 crossings, 1–32)
 *     Pressing pad N lights pads 0..N. Pressing the active pad cancels loop.
 *   - Step buttons 16–31 → 16-step sequencer gate toggles
 *   - MIDI clock (0xF8) → BPM measurement for display
 *   - Display: large "KWAHZOLIN" pixel title, glitch visualizer, loop status,
 *     step sequencer blocks
 *
 * Display layout (128×64, 1-bit):
 *   Glitch layer   scattered blocks reacting to Osc Chaos + Filter Chaos
 *   y=0            top-right corner: "L:N" loop hint when loop is active
 *   y=25..38       "KWAHZOLIN" in 5×7 pixel font at scale 2 (fill_rect)
 *                  (or knob name + value when a knob is being turned)
 *   y=52..62       16 step sequencer blocks (7×11 px each, 8px pitch)
 */

import {
    MidiNoteOn, MidiNoteOff, MidiCC,
    MoveKnob1, MoveKnob8,
    MovePads, MoveSteps
} from '/data/UserData/schwung/shared/constants.mjs';

import {
    isCapacitiveTouchMessage,
    decodeDelta,
    setLED, setButtonLED, clearAllLEDs
} from '/data/UserData/schwung/shared/input_filter.mjs';

/* ====================================================================
 * Constants
 * ==================================================================== */

const KNOB_CC_BASE  = MoveKnob1;   /* 71 */
const KNOB_COUNT    = 8;

const KNOB_KEYS  = ['osc1_rate',  'osc2_rate',  'osc_chaos',     'filter_cutoff',
                    'filter_resonance', 'filter_chaos', 'filter_drive', 'ring_mod'];

/* Full names shown in display during knob feedback */
const KNOB_NAMES = ['Osc 1 Frequency', 'Osc 2 Frequency', 'Osc Chaos',      'Filter Cutoff',
                    'Filter Resonance', 'Filter Chaos',    'Filter Drive',   'Ring Modulation'];

/* Default knob values 0–127 matching DSP defaults */
const KNOB_DEFAULTS = [25, 19, 64, 64, 38, 64, 25, 0];

/* Ticks to show knob feedback (~1.5 sec @ 44 Hz) */
const FEEDBACK_TICKS = 66;

/* MIDI clock */
const TICKS_PER_BEAT  = 24;
const CLOCK_LOSS_TICKS = 88;

/* LED colors */
const COLOR_STEP_ON    = 120;  /* White */
const COLOR_STEP_OFF   = 0;    /* Off   */
const COLOR_PAD_ACTIVE = 11;   /* Neon Green */
const COLOR_PAD_IDLE   = 0;    /* Off        */

/* ====================================================================
 * Pixel font — 5×7 bitmap for each character in "KWAHZOLIN"
 * Each entry is 7 row values. Each row is a 5-bit number where
 * bit 4 = leftmost column, bit 0 = rightmost column.
 * ==================================================================== */

const FONT5x7 = {
    /* K:  X . . . X        W:  X . . . X        A:  . X X X .
           X . . X .            X . . . X             X . . . X
           X . X . .            X . X . X             X . . . X
           X X . . .            X . X . X             X X X X X
           X . X . .            X X . X X             X . . . X
           X . . X .            X X . X X             X . . . X
           X . . . X            X . . . X             X . . . X  */
    K: [17, 18, 20, 24, 20, 18, 17],
    W: [17, 17, 21, 21, 27, 27, 17],
    A: [14, 17, 17, 31, 17, 17, 17],

    /* H:  X . . . X        Z:  X X X X X        O:  . X X X .
           X . . . X            . . . . X             X . . . X
           X . . . X            . . . X .             X . . . X
           X X X X X            . . X . .             X . . . X
           X . . . X            . X . . .             X . . . X
           X . . . X            X . . . .             X . . . X
           X . . . X            X X X X X             . X X X .  */
    H: [17, 17, 17, 31, 17, 17, 17],
    Z: [31,  1,  2,  4,  8, 16, 31],
    O: [14, 17, 17, 17, 17, 17, 14],

    /* L:  X . . . .        I:  X X X X X        N:  X . . . X
           X . . . .            . . X . .             X X . . X
           X . . . .            . . X . .             X . X . X
           X . . . .            . . X . .             X . X . X
           X . . . .            . . X . .             X . . X X
           X . . . .            . . X . .             X . . . X
           X X X X X            X X X X X             X . . . X  */
    L: [16, 16, 16, 16, 16, 16, 31],
    I: [31,  4,  4,  4,  4,  4, 31],
    N: [17, 25, 21, 21, 19, 17, 17],
};

const TITLE_STR  = 'KWAHZOLIN';
const CHAR_SCALE = 2;
const CHAR_W     = 5 * CHAR_SCALE;                                      /* 10px */
const CHAR_H     = 7 * CHAR_SCALE;                                      /* 14px */
const CHAR_GAP   = 2;
const TITLE_W    = TITLE_STR.length * CHAR_W + (TITLE_STR.length - 1) * CHAR_GAP; /* 106px */
const TITLE_X    = (128 - TITLE_W) >> 1;                                /* 11px */
const TITLE_Y    = 25;   /* centered vertically on the 128×64 display  */
const STEPS_Y    = 52;
const STEPS_H    = 11;

/* ====================================================================
 * State
 * ==================================================================== */

const knobValues    = [...KNOB_DEFAULTS];

let activeKnob      = -1;
let feedbackTicks   = 0;

let bpm             = 120;
let clockTickCount  = 0;
let lastClockMs     = 0;
let ticksSinceClock = 0;
let clockActive     = false;

let activePadIndex  = -1;   /* -1 = free running, 0–31 = loop active */
let stepMask        = 0xFFFF;

let displayDirty    = true;
let prevStepMask    = -1;

/* Progressive LED init */
let ledInitPhase    = 0;
let ledInitIndex    = 0;
const LEDS_PER_TICK = 8;

/* ====================================================================
 * Utilities
 * ==================================================================== */

function knobToParam(v)      { return (v / 127.0).toFixed(4); }
function sendKnobParam(idx)  { host_module_set_param(KNOB_KEYS[idx], knobToParam(knobValues[idx])); }
function knobValueDisplay(i) { return `${Math.round(knobValues[i] / 1.27)}%`; }

/* ====================================================================
 * LED helpers
 * ==================================================================== */

/**
 * Light all pads from index 0 up to and including activePadIndex,
 * so the number of lit pads visually shows the loop length.
 * All pads off when activePadIndex = -1 (free running).
 */
function refreshPadLEDs() {
    for (let i = 0; i < 32; i++) {
        setLED(MovePads[i], i <= activePadIndex ? COLOR_PAD_ACTIVE : COLOR_PAD_IDLE);
    }
}

function refreshStepLEDs() {
    for (let i = 0; i < 16; i++) {
        setLED(MoveSteps[i], (stepMask >> i) & 1 ? COLOR_STEP_ON : COLOR_STEP_OFF);
    }
}

/* Progressive LED init to avoid flooding the ~64-packet buffer */
function initLEDsBatch() {
    if (ledInitPhase === 0) {
        const end = Math.min(ledInitIndex + LEDS_PER_TICK, 32);
        for (let i = ledInitIndex; i < end; i++) {
            setLED(MovePads[i], COLOR_PAD_IDLE);
        }
        ledInitIndex = end;
        if (ledInitIndex >= 32) { ledInitPhase = 1; ledInitIndex = 0; }
    } else if (ledInitPhase === 1) {
        const end = Math.min(ledInitIndex + LEDS_PER_TICK, 16);
        for (let i = ledInitIndex; i < end; i++) {
            setLED(MoveSteps[i], (stepMask >> i) & 1 ? COLOR_STEP_ON : COLOR_STEP_OFF);
        }
        ledInitIndex = end;
        if (ledInitIndex >= 16) { ledInitPhase = 2; }
    }
}

/* ====================================================================
 * BPM helpers
 * ==================================================================== */

function handleClockTick() {
    ticksSinceClock = 0;
    clockActive     = true;

    clockTickCount++;
    if (clockTickCount >= TICKS_PER_BEAT) {
        clockTickCount = 0;
        const now = Date.now();
        if (lastClockMs > 0) {
            const interval = now - lastClockMs;
            if (interval > 0) {
                const measured = 60000 / interval;
                if (measured >= 20 && measured <= 300) bpm = measured;
            }
        }
        lastClockMs = now;
    }
}

function readHostBPM() {
    try {
        const v = parseInt(host_get_setting('tempo_bpm'), 10);
        if (v >= 20 && v <= 300) return v;
    } catch (_) { /* ignore */ }
    return 120;
}

/* ====================================================================
 * Loop control
 * ==================================================================== */

/**
 * Arm a loop of (padIndex + 1) Osc2 crossings, or cancel if the
 * currently active pad (the last lit pad) is pressed again.
 */
function setLoop(padIndex) {
    if (padIndex === activePadIndex) {
        /* Pressing the active pad: cancel loop → free running */
        activePadIndex = -1;
        host_module_set_param('loop_active', '0');
    } else {
        /* New pad: arm loop at new length */
        activePadIndex = padIndex;
        const steps = padIndex + 1;   /* pad 0 = 1 crossing, pad 31 = 32 crossings */
        host_module_set_param('loop_active', '0');   /* force fresh snapshot */
        host_module_set_param('loop_beats',  String(steps));
        host_module_set_param('loop_active', '1');
    }
    refreshPadLEDs();
    displayDirty = true;
}

/* ====================================================================
 * Step sequencer
 * ==================================================================== */

function toggleStep(stepIndex) {
    stepMask ^= (1 << stepIndex);
    host_module_set_param('step_mask', String(stepMask));
    setLED(MoveSteps[stepIndex], (stepMask >> stepIndex) & 1 ? COLOR_STEP_ON : COLOR_STEP_OFF);
    displayDirty = true;
}

/* ====================================================================
 * Display — pixel font renderer
 * ==================================================================== */

function drawChar(x, y, ch) {
    const rows = FONT5x7[ch];
    if (!rows) return;
    for (let row = 0; row < 7; row++) {
        const bits = rows[row];
        const py = y + row * CHAR_SCALE;
        for (let col = 0; col < 5; col++) {
            if ((bits >> (4 - col)) & 1) {
                fill_rect(x + col * CHAR_SCALE, py, CHAR_SCALE, CHAR_SCALE, 1);
            }
        }
    }
}

function drawTitle() {
    for (let i = 0; i < TITLE_STR.length; i++) {
        drawChar(TITLE_X + i * (CHAR_W + CHAR_GAP), TITLE_Y, TITLE_STR[i]);
    }
}

/* ====================================================================
 * Display — glitch visualizer
 *
 * Reacts to combined Osc Chaos (index 2) + Filter Chaos (index 5).
 * Low chaos: sparse small outline rects, slow feel.
 * High chaos: many large filled blocks, aggressive tearing.
 * Drawn before the title so the title overlays the glitch.
 * ==================================================================== */

function drawGlitch(chaos) {
    if (chaos < 0.03) return;
    const n    = Math.ceil(chaos * 12);
    const maxW = 4  + Math.floor(chaos * 24);
    const maxH = 2  + Math.floor(chaos * 10);
    for (let b = 0; b < n; b++) {
        const bx = Math.floor(Math.random() * 128);
        const by = Math.floor(Math.random() * (STEPS_Y - 2));
        const bw = Math.max(2, Math.floor(Math.random() * maxW));
        const bh = Math.max(1, Math.floor(Math.random() * maxH));
        const cx = Math.min(bw, 128 - bx);
        const cy = Math.min(bh, STEPS_Y - 2 - by);
        if (cx <= 0 || cy <= 0) continue;
        if (chaos > 0.5 && Math.random() < chaos) {
            fill_rect(bx, by, cx, cy, 1);
        } else {
            draw_rect(bx, by, cx, cy, 1);
        }
    }
}

/* ====================================================================
 * Display — step sequencer row
 * ==================================================================== */

function drawStepSequencer() {
    for (let i = 0; i < 16; i++) {
        const x  = i * 8 + 1;
        const on = (stepMask >> i) & 1;
        if (on) {
            fill_rect(x, STEPS_Y, 7, STEPS_H, 1);
        } else {
            draw_rect(x, STEPS_Y, 7, STEPS_H, 1);
        }
    }
}

/* ====================================================================
 * Display — main draw
 * ==================================================================== */

function drawUI() {
    /* Combined chaos level drives the glitch intensity */
    const chaos    = (knobValues[2] + knobValues[5]) / 254.0;
    const glitchOn = chaos > 0.02;
    const smask    = stepMask;

    /* Skip redraw if nothing changed and no glitch animation running */
    if (!displayDirty && !glitchOn && smask === prevStepMask) {
        return;
    }

    clear_screen();

    /* Glitch layer — drawn first so title renders on top of it */
    if (glitchOn) drawGlitch(chaos);

    /* Title area: large "KWAHZOLIN" normally; knob name+value when turning */
    if (activeKnob >= 0) {
        print(2, TITLE_Y,      KNOB_NAMES[activeKnob],       1);
        print(2, TITLE_Y + 12, knobValueDisplay(activeKnob), 1);
    } else {
        drawTitle();
    }

    /* Corner loop hint: small "L:N" at top-right when loop is active */
    if (activePadIndex >= 0) {
        print(100, 1, `L:${activePadIndex + 1}`, 1);
    }

    /* Step sequencer row */
    drawStepSequencer();

    prevStepMask = smask;
    displayDirty = false;
}

/* ====================================================================
 * Module lifecycle
 * ==================================================================== */

globalThis.init = function () {
    for (let i = 0; i < KNOB_COUNT; i++) sendKnobParam(i);
    host_module_set_param('loop_active', '0');
    host_module_set_param('loop_beats',  '4');
    host_module_set_param('step_mask',   String(stepMask));

    bpm = readHostBPM();

    ledInitPhase = 0;
    ledInitIndex = 0;
    displayDirty = true;
};

globalThis.tick = function () {
    if (ledInitPhase < 2) initLEDsBatch();

    if (activeKnob >= 0) {
        feedbackTicks++;
        if (feedbackTicks > FEEDBACK_TICKS) {
            activeKnob   = -1;
            displayDirty = true;
        }
    }

    ticksSinceClock++;
    if (ticksSinceClock > CLOCK_LOSS_TICKS && clockActive) {
        clockActive  = false;
        displayDirty = true;
    }

    drawUI();
};

/* ====================================================================
 * MIDI routing
 * ==================================================================== */

globalThis.onMidiMessageInternal = function (data) {
    if (data[0] === 0xF8) {
        handleClockTick();
        return;
    }

    if (isCapacitiveTouchMessage(data)) return;

    const statusByte = data[0];
    if (statusByte >= 0xF0) return;

    const status   = statusByte & 0xF0;
    const d1       = data[1];
    const d2       = data[2];
    const isNoteOn = status === MidiNoteOn  && d2 > 0;
    const isNote   = status === MidiNoteOn  || status === MidiNoteOff;
    const isCC     = status === MidiCC;

    /* Step buttons (notes 16–31) */
    if (isNote && d1 >= 16 && d1 <= 31 && isNoteOn) {
        toggleStep(d1 - 16);
        return;
    }

    /* Pads (notes 68–99): loop length in Osc2 crossings */
    if (isNote && d1 >= 68 && d1 <= 99 && isNoteOn) {
        setLoop(d1 - 68);
        return;
    }

    /* Knobs (CC 71–78): relative delta */
    if (isCC && d1 >= KNOB_CC_BASE && d1 < KNOB_CC_BASE + KNOB_COUNT) {
        const idx   = d1 - KNOB_CC_BASE;
        const delta = decodeDelta(d2);
        if (delta !== 0) {
            knobValues[idx] = Math.max(0, Math.min(127, knobValues[idx] + delta));
            sendKnobParam(idx);
            activeKnob    = idx;
            feedbackTicks = 0;
            displayDirty  = true;
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function (data) {
    const status = data[0] & 0xF0;
    if (status === MidiNoteOn || status === MidiNoteOff) {
        host_module_send_midi(data, 'external');
    }
};
