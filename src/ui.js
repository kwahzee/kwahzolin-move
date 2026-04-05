/*
 * Kwahzolin — JavaScript UI
 *
 * Handles:
 *   - 8 knobs (CC 71–78, relative delta) → parameters sent to DSP
 *   - Pads 68–99 → loop length control (pad index+1 = beats, 1–32)
 *   - Step buttons 16–31 → 16-step sequencer gate toggles
 *   - MIDI clock (0xF8) → BPM measurement and fallback to host_get_setting
 *   - Display: knob feedback / loop status / step sequencer blocks
 *
 * Display layout (128×64, 1-bit):
 *   y=2:   Module name or active knob name
 *   y=13:  Knob value (when feedback active) or loop status
 *   y=24:  BPM info
 *   y=40:  16-step blocks (7×12 px each)
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

const KNOB_KEYS  = ['osc1_rate', 'osc2_rate', 'chaos', 'cutoff',
                     'resonance', 'drive', 'rungler_mod', 'ring_mod'];
const KNOB_NAMES = ['Osc1', 'Osc2', 'Chaos', 'Cutoff',
                     'Reson', 'Drive', 'R.Mod', 'Ring'];

/* Default knob values 0–127 (matching DSP defaults) */
const KNOB_DEFAULTS = [25, 19, 64, 64, 38, 25, 64, 0];

/* How many tick() calls to show knob feedback (~1.5 sec @ 44 Hz) */
const FEEDBACK_TICKS = 66;

/* MIDI clock: 24 ticks per beat */
const TICKS_PER_BEAT = 24;

/* Tick count for clock-loss detection (~2 sec @ 44 Hz) */
const CLOCK_LOSS_TICKS = 88;

/* LED colors for pads and steps */
const COLOR_STEP_ON    = 120;  /* White */
const COLOR_STEP_OFF   = 0;    /* Black (off) */
const COLOR_PAD_ACTIVE = 11;   /* Neon Green */
const COLOR_PAD_IDLE   = 0;    /* Black */
const COLOR_LOOP_ARMED = 8;    /* Bright Green */

/* ====================================================================
 * State
 * ==================================================================== */

/* Knob values 0–127, accumulated from relative deltas */
const knobValues = [...KNOB_DEFAULTS];

/* Knob display feedback */
let activeKnob      = -1;       /* index 0–7, or -1 for none */
let feedbackTicks   = 0;

/* BPM tracking */
let bpm             = 120;
let clockTickCount  = 0;        /* counts 0xF8 ticks within one beat */
let lastClockMs     = 0;        /* real-time of last beat tick */
let ticksSinceClock = 0;        /* tick() calls since last 0xF8 */
let clockActive     = false;

/* Loop state (one pad active at most) */
let activePadIndex  = -1;       /* -1 = free running, 0–31 = loop beats-1 */

/* Step sequencer (16 steps) */
let stepMask        = 0xFFFF;   /* all steps on */

/* Display dirty tracking — redraw only when state changes */
let displayDirty    = true;
let prevTitle       = '';
let prevStatus      = '';
let prevBpmStr      = '';
let prevStepMask    = -1;

/* ====================================================================
 * Utilities
 * ==================================================================== */

/** Convert 0–127 knob value to 0–1 float string for set_param */
function knobToParam(v) {
    return (v / 127.0).toFixed(4);
}

/** Send a knob parameter to the DSP */
function sendKnobParam(idx) {
    host_module_set_param(KNOB_KEYS[idx], knobToParam(knobValues[idx]));
}

/** Build a human-readable value string for display */
function knobValueDisplay(idx) {
    const pct = Math.round(knobValues[idx] / 1.27);
    return `${pct}%`;
}

/* ====================================================================
 * LED helpers
 * ==================================================================== */

function refreshPadLEDs() {
    for (let i = 0; i < 32; i++) {
        const note = MovePads[i];
        setLED(note, i === activePadIndex ? COLOR_PAD_ACTIVE : COLOR_PAD_IDLE);
    }
}

function refreshStepLEDs() {
    for (let i = 0; i < 16; i++) {
        const note = MoveSteps[i];
        const on   = (stepMask >> i) & 1;
        setLED(note, on ? COLOR_STEP_ON : COLOR_STEP_OFF);
    }
}

/* Progressive LED init to avoid overflowing the ~64-packet buffer */
let ledInitPhase    = 0;  /* 0=pads, 1=steps, 2=done */
let ledInitIndex    = 0;
const LEDS_PER_TICK = 8;

function initLEDsBatch() {
    if (ledInitPhase === 0) {
        /* Init pads in batches */
        const end = Math.min(ledInitIndex + LEDS_PER_TICK, 32);
        for (let i = ledInitIndex; i < end; i++) {
            setLED(MovePads[i], COLOR_PAD_IDLE);
        }
        ledInitIndex = end;
        if (ledInitIndex >= 32) { ledInitPhase = 1; ledInitIndex = 0; }
    } else if (ledInitPhase === 1) {
        /* Init steps in batches */
        const end = Math.min(ledInitIndex + LEDS_PER_TICK, 16);
        for (let i = ledInitIndex; i < end; i++) {
            const on = (stepMask >> i) & 1;
            setLED(MoveSteps[i], on ? COLOR_STEP_ON : COLOR_STEP_OFF);
        }
        ledInitIndex = end;
        if (ledInitIndex >= 16) { ledInitPhase = 2; }
    }
}

/* ====================================================================
 * BPM helpers
 * ==================================================================== */

/** Called on each MIDI 0xF8 message */
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
                if (measured >= 20 && measured <= 300) {
                    bpm = measured;
                    /* If a loop is active, re-send loop_beats so DSP recomputes
                       loop_samples on its next block with the current BPM.
                       DSP does this automatically via host->get_bpm(), but updating
                       the beat count forces a re-snapshot if length changed. */
                }
            }
        }
        lastClockMs = now;
    }
}

/** Read fallback BPM from host settings */
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

/** Arm or disarm a loop. padIndex 0–31, or -1 to disarm. */
function setLoop(padIndex) {
    if (padIndex === activePadIndex) {
        /* Same pad: toggle off → free running */
        activePadIndex = -1;
        host_module_set_param('loop_active', '0');
    } else {
        /* New pad: disarm old, arm new */
        activePadIndex = padIndex;
        const beats = padIndex + 1;   /* pad 0 = 1 beat, pad 31 = 32 beats */
        host_module_set_param('loop_active', '0');   /* ensure fresh snapshot */
        host_module_set_param('loop_beats',  String(beats));
        host_module_set_param('loop_active', '1');
    }
    refreshPadLEDs();
    displayDirty = true;
}

/* ====================================================================
 * Step sequencer
 * ==================================================================== */

/** Toggle step bit and send to DSP */
function toggleStep(stepIndex) {
    stepMask ^= (1 << stepIndex);
    host_module_set_param('step_mask', String(stepMask));
    const on = (stepMask >> stepIndex) & 1;
    setLED(MoveSteps[stepIndex], on ? COLOR_STEP_ON : COLOR_STEP_OFF);
    displayDirty = true;
}

/* ====================================================================
 * Display
 * ==================================================================== */

function drawStepSequencer() {
    /* 16 blocks × 8px wide = 128px. Each block: 7px wide, 12px tall at y=40 */
    for (let i = 0; i < 16; i++) {
        const x  = i * 8 + 1;
        const on = (stepMask >> i) & 1;
        if (on) {
            fill_rect(x, 40, 7, 12, 1);
        } else {
            draw_rect(x, 40, 7, 12, 1);
        }
    }
}

function buildTitle() {
    if (activeKnob >= 0) return KNOB_NAMES[activeKnob];
    return 'KWAHZOLIN';
}

function buildStatus() {
    if (activeKnob >= 0) return knobValueDisplay(activeKnob);
    if (activePadIndex >= 0) {
        const beats = activePadIndex + 1;
        return `LOOP ${beats}b`;
    }
    return 'FREE';
}

function buildBPMStr() {
    if (clockActive) {
        return `${Math.round(bpm)} BPM`;
    }
    /* Fallback: read from host settings */
    return `${readHostBPM()} BPM`;
}

function drawUI() {
    const title   = buildTitle();
    const status  = buildStatus();
    const bpmStr  = buildBPMStr();
    const smask   = stepMask;

    /* Only redraw if something changed */
    if (!displayDirty
        && title  === prevTitle
        && status === prevStatus
        && bpmStr === prevBpmStr
        && smask  === prevStepMask) {
        return;
    }

    clear_screen();

    print(2, 2,  title,  1);
    print(2, 13, status, 1);
    print(2, 24, bpmStr, 1);
    drawStepSequencer();

    prevTitle    = title;
    prevStatus   = status;
    prevBpmStr   = bpmStr;
    prevStepMask = smask;
    displayDirty = false;
}

/* ====================================================================
 * Module lifecycle
 * ==================================================================== */

globalThis.init = function () {
    /* Push defaults to DSP */
    for (let i = 0; i < KNOB_COUNT; i++) sendKnobParam(i);
    host_module_set_param('loop_active', '0');
    host_module_set_param('loop_beats',  '4');
    host_module_set_param('step_mask',   String(stepMask));

    /* Seed BPM from host settings */
    bpm = readHostBPM();

    /* Start progressive LED init */
    ledInitPhase = 0;
    ledInitIndex = 0;
    displayDirty = true;
};

globalThis.tick = function () {
    /* Continue LED initialisation over first few frames */
    if (ledInitPhase < 2) {
        initLEDsBatch();
    }

    /* Advance feedback timer */
    if (activeKnob >= 0) {
        feedbackTicks++;
        if (feedbackTicks > FEEDBACK_TICKS) {
            activeKnob   = -1;
            displayDirty = true;
        }
    }

    /* Clock-loss detection */
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
    /* Bypass noise filter only for MIDI clock — handle it first */
    if (data[0] === 0xF8) {
        handleClockTick();
        return;
    }

    /* Drop capacitive touch events (notes 0–9) */
    if (isCapacitiveTouchMessage(data)) return;

    /* Drop other system messages */
    const statusByte = data[0];
    if (statusByte >= 0xF0) return;

    const status   = statusByte & 0xF0;
    const d1       = data[1];
    const d2       = data[2];
    const isNoteOn = status === MidiNoteOn  && d2 > 0;
    const isNote   = status === MidiNoteOn  || status === MidiNoteOff;
    const isCC     = status === MidiCC;

    /* ---- Step buttons (notes 16–31) ---- */
    if (isNote && d1 >= 16 && d1 <= 31 && isNoteOn) {
        toggleStep(d1 - 16);
        return;
    }

    /* ---- Pads (notes 68–99): loop length ---- */
    if (isNote && d1 >= 68 && d1 <= 99 && isNoteOn) {
        setLoop(d1 - 68);   /* 0–31 = 1–32 beats */
        return;
    }

    /* ---- Knobs (CC 71–78): relative delta ---- */
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
    /* Pass-through: forward external MIDI as notes to the DSP */
    const status = data[0] & 0xF0;
    if (status === MidiNoteOn || status === MidiNoteOff) {
        host_module_send_midi(data, 'external');
    }
};
