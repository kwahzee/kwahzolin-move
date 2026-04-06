import { MoveKnob1 } from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta, isCapacitiveTouchMessage } from '/data/UserData/schwung/shared/input_filter.mjs';

const KNOB_CC_BASE = MoveKnob1;
const KNOB_COUNT   = 8;

const KNOB_KEYS = [
    'osc1_freq', 'osc2_freq', 'osc_chaos', 'filter_cutoff',
    'filter_resonance', 'filter_lfo', 'filter_chaos', 'loop'
];

const KNOB_NAMES = [
    'Osc 1 Frequency', 'Osc 2 Frequency', 'Osc Chaos',     'Filter Cutoff',
    'Filter Resonance', 'Filter LFO',     'Filter Chaos',  'Loop'
];

const KNOB_DEFAULTS = [49, 58, 38, 78, 64, 0, 51, 0];

const FEEDBACK_TICKS = 88;

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
const CHAR_W     = 5 * CHAR_SCALE;
const CHAR_H     = 7 * CHAR_SCALE;
const CHAR_GAP   = 2;
const TITLE_W    = TITLE_STR.length * CHAR_W + (TITLE_STR.length - 1) * CHAR_GAP;
const TITLE_X    = Math.floor((128 - TITLE_W) / 2);
const TITLE_Y    = Math.floor((64  - CHAR_H)  / 2);

const knobValues = [...KNOB_DEFAULTS];

let activeKnob    = -1;
let feedbackTicks = 0;
let displayDirty  = true;

function knobToParam(v)     { return (v / 127).toFixed(4); }
function sendKnobParam(idx) { host_module_set_param(KNOB_KEYS[idx], knobToParam(knobValues[idx])); }
function knobValuePct(i)    { return `${Math.round(knobValues[i] / 1.27)}%`; }

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

globalThis.onMidiMessageInternal = function (data) {
    if (isCapacitiveTouchMessage(data)) return;
    if ((data[0] & 0xF0) !== 0xB0) return;

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
