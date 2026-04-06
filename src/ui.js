/*
 * kwahzolin v0.1.4 — JavaScript UI
 *
 * Fully autonomous Benjolin synthesizer. 8 knobs only.
 * No pads. No LEDs. No sequencer. No MIDI notes.
 *
 * Knobs (CC 71–78):
 *   1: Osc 1 Frequency    5: Filter Resonance
 *   2: Osc 2 Frequency    6: Filter Drive
 *   3: Osc Chaos          7: Filter Chaos
 *   4: Filter Cutoff      8: Loop
 *
 * Display (128×64):
 *   Large "KWAHZOLIN" pixel title — normally.
 *   Knob name + value for ~2 seconds when a knob is turned.
 */

import { MoveKnob1 } from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta, isCapacitiveTouchMessage } from '/data/UserData/schwung/shared/input_filter.mjs';

/* ====================================================================
 * Constants
 * ==================================================================== */

const KNOB_CC_BASE = MoveKnob1;   /* 71 */
const KNOB_COUNT   = 8;

const KNOB_KEYS = [
    'osc1_freq', 'osc2_freq', 'osc_chaos', 'filter_cutoff',
    'filter_resonance', 'filter_drive', 'filter_chaos', 'loop'
];

const KNOB_NAMES = [
    'Osc 1 Frequency', 'Osc 2 Frequency', 'Osc Chaos',      'Filter Cutoff',
    'Filter Resonance', 'Filter Drive',    'Filter Chaos',   'Loop'
];

/* Default knob positions 0–127, matching DSP defaults:
 *   110 Hz  → p≈0.386 → 49
 *   170 Hz  → p≈0.456 → 58
 *   chaos   0.3       → 38
 *   800 Hz  → p=0.5   → 64
 *   res     0.5       → 64
 *   drive   0.2       → 25
 *   fchaos  0.4       → 51
 *   loop    0.0       → 0
 */
const KNOB_DEFAULTS = [49, 58, 38, 64, 64, 25, 51, 0];

/* Ticks to show knob feedback (~2 sec @ ~44 Hz tick rate) */
const FEEDBACK_TICKS = 88;

/* ====================================================================
 * Pixel font — 5×7 bitmap for each character in "KWAHZOLIN"
 * Each row is a 5-bit value: bit4 = leftmost, bit0 = rightmost.
 * ==================================================================== */

const FONT5x7 = {
    K: [17, 18, 20, 24, 20, 18, 17],
    W: [17, 17, 21, 21, 27, 27, 17],
    A: [14, 17, 17, 31, 17, 17, 17],
    H: [17, 17, 17, 31, 17, 17, 17],
    Z: [31,  1,  2,  4,  8, 16, 31],
    O: [14, 17, 17, 17, 17, 17, 14],
    L: [16, 16, 16, 16, 16, 16, 31],
    I: [31,  4,  4,  4,  4,  4, 31],
    N: [17, 25, 21, 21, 19, 17, 17],
};

const TITLE_STR  = 'KWAHZOLIN';
const CHAR_SCALE = 2;
const CHAR_W     = 5 * CHAR_SCALE;                                       /* 10px */
const CHAR_H     = 7 * CHAR_SCALE;                                       /* 14px */
const CHAR_GAP   = 2;
const TITLE_W    = TITLE_STR.length * CHAR_W + (TITLE_STR.length - 1) * CHAR_GAP;
const TITLE_X    = Math.floor((128 - TITLE_W) / 2);
const TITLE_Y    = Math.floor((64  - CHAR_H)  / 2);

/* ====================================================================
 * State
 * ==================================================================== */

const knobValues = [...KNOB_DEFAULTS];

let activeKnob    = -1;
let feedbackTicks = 0;
let displayDirty  = true;

/* ====================================================================
 * Knob helpers
 * ==================================================================== */

function knobToParam(v)     { return (v / 127).toFixed(4); }
function sendKnobParam(idx) { host_module_set_param(KNOB_KEYS[idx], knobToParam(knobValues[idx])); }
function knobValuePct(i)    { return `${Math.round(knobValues[i] / 1.27)}%`; }

/* ====================================================================
 * Display
 * ==================================================================== */

function drawChar(x, y, ch) {
    const rows = FONT5x7[ch];
    if (!rows) return;
    for (let row = 0; row < 7; row++) {
        const bits = rows[row];
        const py   = y + row * CHAR_SCALE;
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

function drawUI() {
    if (!displayDirty) return;

    clear_screen();

    if (activeKnob >= 0) {
        print(2, 22, KNOB_NAMES[activeKnob], 1);
        print(2, 36, knobValuePct(activeKnob), 1);
    } else {
        drawTitle();
    }

    displayDirty = false;
}

/* ====================================================================
 * Module lifecycle
 * ==================================================================== */

globalThis.init = function () {
    for (let i = 0; i < KNOB_COUNT; i++) sendKnobParam(i);
    displayDirty = true;
};

globalThis.tick = function () {
    if (activeKnob >= 0) {
        feedbackTicks++;
        if (feedbackTicks > FEEDBACK_TICKS) {
            activeKnob   = -1;
            displayDirty = true;
        }
    }
    drawUI();
};

/* ====================================================================
 * MIDI — knobs (CC 71–78) only. Everything else ignored.
 * ==================================================================== */

globalThis.onMidiMessageInternal = function (data) {
    if (isCapacitiveTouchMessage(data)) return;
    if ((data[0] & 0xF0) !== 0xB0) return;   /* CC messages only */

    const cc  = data[1];
    const val = data[2];

    if (cc < KNOB_CC_BASE || cc >= KNOB_CC_BASE + KNOB_COUNT) return;

    const idx   = cc - KNOB_CC_BASE;
    const delta = decodeDelta(val);
    if (delta !== 0) {
        knobValues[idx] = Math.max(0, Math.min(127, knobValues[idx] + delta));
        sendKnobParam(idx);
        activeKnob    = idx;
        feedbackTicks = 0;
        displayDirty  = true;
    }
};
