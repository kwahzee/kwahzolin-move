import { MoveKnob1, MoveShift, MoveBack, MoveMainKnob, MoveMainButton }
    from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta, isCapacitiveTouchMessage }
    from '/data/UserData/schwung/shared/input_filter.mjs';

const KNOB_CC_BASE = MoveKnob1;
const KNOB_COUNT   = 8;
const CC_JOG_WHEEL = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_BACK      = MoveBack;

const STATE_MAIN         = 0;
const STATE_LFO_SELECT   = 1;
const STATE_LFO_SETTINGS = 2;
const STATE_DISTORTION   = 3;
const STATE_KNOB_DISPLAY = 4;

const LFO_SHAPES  = ['Triangle', 'Sine', 'Square', 'Sawtooth', 'Random'];
const LFO_TARGETS = [
    'Osc 1 Freq', 'Osc 2 Freq', 'Osc Chaos',
    'Filter Cutoff', 'Filter Res', 'Filter Chaos',
    'Cross Mod', 'Loop'
];
const LFO_PROPS = ['SHAPE', 'RATE', 'AMOUNT', 'TARGET'];

const DIST_TYPE_NAMES = ['Overdrive', 'Distortion', 'Fuzz'];

const KNOB_KEYS = [
    'osc1_freq', 'osc2_freq', 'osc_chaos', 'filter_cutoff',
    'filter_resonance', 'filter_chaos', 'cross_mod', 'loop'
];
const KNOB_NAMES = [
    'Osc 1 Frequency', 'Osc 2 Frequency', 'Osc Chaos',    'Filter Cutoff',
    'Filter Resonance', 'Filter Chaos',   'Cross Mod', 'Loop'
];
const KNOB_DEFAULTS = [49, 58, 0, 78, 0, 0, 0, 0];

const STATE_FILE = '/data/UserData/schwung/modules/sound_generators/kwahzolin/state.json';

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
const CHAR_GAP   = 2;
const TITLE_W    = TITLE_STR.length * CHAR_W + (TITLE_STR.length - 1) * CHAR_GAP;
const TITLE_X    = Math.floor((128 - TITLE_W) / 2);

const knobValues = [...KNOB_DEFAULTS];

const lfoSettings = [
    { shape: 0, rate: 0.5, amount: 0.0, target: 3 },
    { shape: 0, rate: 0.5, amount: 0.0, target: 3 },
    { shape: 0, rate: 0.5, amount: 0.0, target: 3 },
];

let distEnabled = false;
let distType    = 1;
let distAmount  = 0.0;
let distMix     = 1.0;

let menuState         = STATE_MAIN;
let prevMenuState     = STATE_MAIN;
let mainCursor        = 0;
let lfoSelectCursor   = 0;
let activeLfo         = 0;
let lfoSettingsCursor = 0;
let distCursor        = 0;
let editMode          = false;
let activeKnob        = -1;
let knobTicks         = 0;
let displayDirty      = true;

const KNOB_TIMEOUT_TICKS = 44;

function knobToParam(v)     { return (v / 127).toFixed(4); }
function sendKnobParam(idx) { host_module_set_param(KNOB_KEYS[idx], knobToParam(knobValues[idx])); }
function knobValuePct(i)    { return `${Math.round(knobValues[i] / 1.27)}%`; }

function sendLfoParams(i) {
    const n = i + 1;
    host_module_set_param(`lfo${n}_rate`,   lfoSettings[i].rate.toFixed(4));
    host_module_set_param(`lfo${n}_amount`, lfoSettings[i].amount.toFixed(4));
    host_module_set_param(`lfo${n}_shape`,  String(lfoSettings[i].shape));
    host_module_set_param(`lfo${n}_target`, String(lfoSettings[i].target));
}

function sendDistParams() {
    host_module_set_param('dist_type',   String(distEnabled ? distType : 0));
    host_module_set_param('dist_amount', distAmount.toFixed(4));
    host_module_set_param('dist_mix',    distMix.toFixed(4));
}

function saveState() {
    const s = {
        lfo: lfoSettings.map(l => ({ rate: l.rate, amount: l.amount, shape: l.shape, target: l.target })),
        distEnabled,
        distType,
        distAmount,
        distMix,
    };
    host_write_file(STATE_FILE, JSON.stringify(s));
}

function restoreState() {
    try {
        const raw = host_read_file(STATE_FILE);
        if (!raw) return;
        const s = JSON.parse(raw);
        if (s.lfo && Array.isArray(s.lfo)) {
            for (let i = 0; i < 3 && i < s.lfo.length; i++) {
                const src = s.lfo[i];
                if (typeof src.rate   === 'number') lfoSettings[i].rate   = Math.max(0.05, Math.min(10.0, src.rate));
                if (typeof src.amount === 'number') lfoSettings[i].amount = Math.max(0, Math.min(1, src.amount));
                if (typeof src.shape  === 'number') lfoSettings[i].shape  = Math.max(0, Math.min(4, src.shape|0));
                if (typeof src.target === 'number') lfoSettings[i].target = Math.max(0, Math.min(7, src.target|0));
            }
        }
        if (typeof s.distEnabled === 'boolean') distEnabled = s.distEnabled;
        if (typeof s.distType    === 'number')  distType    = Math.max(1, Math.min(3, s.distType|0));
        if (typeof s.distAmount  === 'number')  distAmount  = Math.max(0, Math.min(1, s.distAmount));
        if (typeof s.distMix     === 'number')  distMix     = Math.max(0, Math.min(1, s.distMix));
    } catch (e) {}
}

function clampI(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
function clampF(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

function editLfoProp(dir) {
    const L = lfoSettings[activeLfo];
    switch (lfoSettingsCursor) {
        case 0: L.shape  = clampI(L.shape + dir, 0, LFO_SHAPES.length - 1); break;
        case 1: L.rate   = clampF(L.rate * (dir > 0 ? 1.12 : 0.893), 0.05, 10.0); break;
        case 2: L.amount = clampF(Math.round((L.amount + dir * 0.05) * 100) / 100, 0.0, 1.0); break;
        case 3: L.target = clampI(L.target + dir, 0, LFO_TARGETS.length - 1); break;
    }
    sendLfoParams(activeLfo);
    saveState();
    displayDirty = true;
}

function editDistProp(dir) {
    switch (distCursor) {
        case 0: distType   = clampI(distType + dir, 1, 3); break;
        case 1: distAmount = clampF(Math.round((distAmount + dir * 0.05) * 100) / 100, 0.0, 1.0); break;
        case 2: distMix    = clampF(Math.round((distMix    + dir * 0.05) * 100) / 100, 0.0, 1.0); break;
    }
    sendDistParams();
    saveState();
    displayDirty = true;
}

function handleJog(delta) {
    const dir = delta > 0 ? 1 : -1;

    if (menuState === STATE_KNOB_DISPLAY) return;

    if (menuState === STATE_MAIN) {
        mainCursor = (mainCursor + dir + 2) % 2;
        displayDirty = true;
        return;
    }

    if (menuState === STATE_LFO_SELECT) {
        lfoSelectCursor = (lfoSelectCursor + dir + 3) % 3;
        displayDirty = true;
        return;
    }

    if (menuState === STATE_LFO_SETTINGS) {
        if (editMode) {
            editLfoProp(dir);
        } else {
            lfoSettingsCursor = (lfoSettingsCursor + dir + LFO_PROPS.length) % LFO_PROPS.length;
            displayDirty = true;
        }
        return;
    }

    if (menuState === STATE_DISTORTION) {
        if (editMode && distCursor < 3) {
            editDistProp(dir);
        } else {
            distCursor = (distCursor + dir + 4) % 4;
            displayDirty = true;
        }
        return;
    }
}

function handleClick() {
    if (menuState === STATE_KNOB_DISPLAY) return;

    if (menuState === STATE_MAIN) {
        if (mainCursor === 0) {
            menuState = STATE_LFO_SELECT;
            lfoSelectCursor = 0;
        } else {
            menuState = STATE_DISTORTION;
            distCursor = 0;
            editMode = false;
        }
        displayDirty = true;
        return;
    }

    if (menuState === STATE_LFO_SELECT) {
        activeLfo = lfoSelectCursor;
        menuState = STATE_LFO_SETTINGS;
        lfoSettingsCursor = 0;
        editMode = false;
        displayDirty = true;
        return;
    }

    if (menuState === STATE_LFO_SETTINGS) {
        editMode = !editMode;
        displayDirty = true;
        return;
    }

    if (menuState === STATE_DISTORTION) {
        if (distCursor === 3) {
            distEnabled = !distEnabled;
            sendDistParams();
            saveState();
        } else {
            editMode = !editMode;
        }
        displayDirty = true;
        return;
    }
}

function handleBack() {
    if (editMode) {
        editMode = false;
        displayDirty = true;
        return;
    }

    if (menuState === STATE_LFO_SETTINGS) {
        menuState = STATE_LFO_SELECT;
        displayDirty = true;
        return;
    }

    if (menuState === STATE_LFO_SELECT || menuState === STATE_DISTORTION) {
        menuState = STATE_MAIN;
        displayDirty = true;
        return;
    }
}

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
        drawChar(TITLE_X + i * (CHAR_W + CHAR_GAP), 0, TITLE_STR[i]);
    }
}

function drawSeparator(y) {
    fill_rect(0, y, 128, 1, 1);
}

function drawMain() {
    clear_screen();
    drawTitle();
    drawSeparator(16);
    const items = ['LFO', 'DISTORTION'];
    for (let i = 0; i < items.length; i++) {
        const y = 22 + i * 16;
        if (i === mainCursor) print(2, y, '>', 1);
        print(12, y, items[i], 1);
    }
}

function drawLfoSelect() {
    clear_screen();
    print(2, 1, 'LFO', 1);
    drawSeparator(11);
    for (let i = 0; i < 3; i++) {
        const y = 14 + i * 14;
        if (i === lfoSelectCursor) print(2, y, '>', 1);
        print(12, y, `LFO ${i + 1}`, 1);
    }
}

function drawLfoSettings() {
    clear_screen();
    print(2, 1, `LFO ${activeLfo + 1}`, 1);
    drawSeparator(11);
    const L = lfoSettings[activeLfo];
    const vals = [
        LFO_SHAPES[L.shape],
        `${L.rate.toFixed(2)} Hz`,
        L.amount.toFixed(2),
        LFO_TARGETS[L.target],
    ];
    for (let i = 0; i < LFO_PROPS.length; i++) {
        const y = 14 + i * 12;
        if (i === lfoSettingsCursor) print(2, y, editMode ? '*' : '>', 1);
        print(12, y, `${LFO_PROPS[i]}: ${vals[i]}`, 1);
    }
}

function drawDistortion() {
    clear_screen();
    print(2, 1, 'DISTORTION', 1);
    drawSeparator(11);
    const rows = [
        `TYPE:   ${DIST_TYPE_NAMES[distType - 1]}`,
        `AMOUNT: ${distAmount.toFixed(2)}`,
        `MIX:    ${distMix.toFixed(2)}`,
        distEnabled ? '[ ON  ]' : '[ OFF ]',
    ];
    for (let i = 0; i < rows.length; i++) {
        const y = 14 + i * 12;
        if (i === distCursor) print(2, y, (editMode && i < 3) ? '*' : '>', 1);
        print(12, y, rows[i], 1);
    }
}

function drawKnobDisplay() {
    clear_screen();
    if (activeKnob < 0) return;
    const name = KNOB_NAMES[activeKnob];
    const tw = text_width(name);
    print(Math.floor((128 - tw) / 2), 16, name, 1);

    const pct = knobValuePct(activeKnob);
    const ptw = text_width(pct);
    print(Math.floor((128 - ptw) / 2), 32, pct, 1);

    const barW = 100;
    const barX = Math.floor((128 - barW) / 2);
    const fill = Math.round(knobValues[activeKnob] / 127 * barW);
    draw_rect(barX, 44, barW, 8, 1);
    if (fill > 0) fill_rect(barX, 44, fill, 8, 1);
}

function drawCurrentState() {
    switch (menuState) {
        case STATE_MAIN:         drawMain();         break;
        case STATE_LFO_SELECT:   drawLfoSelect();    break;
        case STATE_LFO_SETTINGS: drawLfoSettings();  break;
        case STATE_DISTORTION:   drawDistortion();   break;
        case STATE_KNOB_DISPLAY: drawKnobDisplay();  break;
    }
}

globalThis.init = function () {
    restoreState();
    for (let i = 0; i < KNOB_COUNT; i++) sendKnobParam(i);
    for (let i = 0; i < 3; i++) sendLfoParams(i);
    sendDistParams();
    displayDirty = true;
};

globalThis.tick = function () {
    if (activeKnob >= 0) {
        knobTicks++;
        if (knobTicks > KNOB_TIMEOUT_TICKS) {
            activeKnob = -1;
            menuState = prevMenuState;
            displayDirty = true;
        }
    }
    if (displayDirty) {
        drawCurrentState();
        displayDirty = false;
    }
};

globalThis.onMidiMessageInternal = function (data) {
    if (isCapacitiveTouchMessage(data)) return;
    if ((data[0] & 0xF0) !== 0xB0) return;

    const cc  = data[1];
    const val = data[2];

    if (cc >= KNOB_CC_BASE && cc < KNOB_CC_BASE + KNOB_COUNT) {
        const delta = decodeDelta(val);
        if (delta !== 0) {
            const idx = cc - KNOB_CC_BASE;
            knobValues[idx] = Math.max(0, Math.min(127, knobValues[idx] + delta));
            sendKnobParam(idx);
            activeKnob = idx;
            knobTicks  = 0;
            if (menuState !== STATE_KNOB_DISPLAY) prevMenuState = menuState;
            menuState = STATE_KNOB_DISPLAY;
            displayDirty = true;
        }
        return;
    }

    if (cc === CC_JOG_WHEEL) {
        const delta = decodeDelta(val);
        if (delta !== 0) handleJog(delta);
        return;
    }

    if (val === 0) return;

    if      (cc === CC_BACK)      handleBack();
    else if (cc === CC_JOG_CLICK) handleClick();
};
