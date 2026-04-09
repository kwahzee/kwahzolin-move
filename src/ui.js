import { MoveKnob1, MoveShift, MoveBack, MoveMainKnob, MoveMainButton }
    from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta, isCapacitiveTouchMessage }
    from '/data/UserData/schwung/shared/input_filter.mjs';

/* ── Hardware CC constants ──────────────────────────────────────────────── */
const KNOB_CC_BASE = MoveKnob1;   /* 71 */
const KNOB_COUNT   = 8;
const CC_JOG_WHEEL = MoveMainKnob;    /* 14 */
const CC_JOG_CLICK = MoveMainButton;  /* 3  */
const CC_BACK      = MoveBack;        /* 51 */
const CC_SHIFT     = MoveShift;       /* 49 */

/* ── Display state machine ──────────────────────────────────────────────── */
const STATE_MENU_MAIN      = 0;
const STATE_MENU_LFO       = 1;
const STATE_MENU_DIST      = 2;
const STATE_MENU_MODULE    = 3;
const STATE_KNOB_DISPLAY   = 4;
const STATE_CONFIRM_UNLOAD = 5;

/* ── LFO config ─────────────────────────────────────────────────────────── */
const LFO_SHAPES  = ['Triangle', 'Sine', 'Square', 'Sawtooth', 'S&H', 'Wander'];
const LFO_TARGETS = [
    'Osc 1 Freq', 'Osc 2 Freq', 'Osc Chaos',
    'Filter Cutoff', 'Filter Res', 'Filter Chaos',
    'Ring Mod', 'Loop'
];
const LFO_PROPS = ['SHAPE', 'RATE', 'AMOUNT', 'TARGET'];

/* ── Distortion config ──────────────────────────────────────────────────── */
const DIST_TYPE_NAMES = ['Overdrive', 'Distortion', 'Fuzz'];

/* ── Knob config ────────────────────────────────────────────────────────── */
const KNOB_KEYS = [
    'osc1_freq', 'osc2_freq', 'osc_chaos', 'filter_cutoff',
    'filter_resonance', 'filter_chaos', 'ring_mod', 'loop'
];
const KNOB_NAMES = [
    'Osc 1 Frequency', 'Osc 2 Frequency', 'Osc Chaos',    'Filter Cutoff',
    'Filter Resonance', 'Filter Chaos',   'Ring Modulation', 'Loop'
];
const KNOB_DEFAULTS = [49, 58, 0, 78, 0, 0, 0, 0];

/* ── Persistence ────────────────────────────────────────────────────────── */
const STATE_FILE = '/data/UserData/schwung/modules/sound_generators/kwahzolin/state.json';

/* ── Title bitmap font ──────────────────────────────────────────────────── */
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

/* ── State ──────────────────────────────────────────────────────────────── */
const knobValues = [...KNOB_DEFAULTS];

/* LFO state (JS mirror — sent to DSP via set_param on change) */
const lfo = [
    { rate: 0.5, amount: 0.0, shape: 0, target: 3 },
    { rate: 0.5, amount: 0.0, shape: 0, target: 3 },
    { rate: 0.5, amount: 0.0, shape: 0, target: 3 },
];

/* Distortion state (JS mirror) */
let distEnabled = false;
let distType    = 1;   /* 1=Overdrive, 2=Distortion, 3=Fuzz */
let distAmount  = 0.0;

/* Navigation state */
let state        = STATE_MENU_MAIN;
let prevState    = STATE_MENU_MAIN;
let mainSel      = 0;               /* 0=LFO, 1=DISTORTION, 2=MODULE */
let lfoTab       = 0;               /* 0-2 active LFO */
let lfoSel       = 0;               /* 0-3 cursor row */
let lfoEditing   = false;
let distSel      = 0;               /* 0-2 cursor row */
let distEditing  = false;
let moduleSel    = 0;               /* 0-1 cursor row */
let confirmSel   = 0;               /* 0=YES, 1=NO */
let activeKnob   = -1;
let knobTicks    = 0;
let shiftHeld    = false;
let displayDirty = true;

const KNOB_TIMEOUT_TICKS = 88; /* ~2 seconds at ~44 ticks/sec */

/* ── Parameter sending ──────────────────────────────────────────────────── */
function knobToParam(v)     { return (v / 127).toFixed(4); }
function sendKnobParam(idx) { host_module_set_param(KNOB_KEYS[idx], knobToParam(knobValues[idx])); }
function knobValuePct(i)    { return `${Math.round(knobValues[i] / 1.27)}%`; }

function sendLfoParams(i) {
    const n = i + 1;
    host_module_set_param(`lfo${n}_rate`,   lfo[i].rate.toFixed(4));
    host_module_set_param(`lfo${n}_amount`, lfo[i].amount.toFixed(4));
    host_module_set_param(`lfo${n}_shape`,  String(lfo[i].shape));
    host_module_set_param(`lfo${n}_target`, String(lfo[i].target));
}

function sendDistParams() {
    host_module_set_param('dist_type',   String(distEnabled ? distType : 0));
    host_module_set_param('dist_amount', distAmount.toFixed(4));
}

/* ── Persistence: save / restore ────────────────────────────────────────── */
function saveState() {
    const s = {
        lfo: lfo.map(l => ({ rate: l.rate, amount: l.amount, shape: l.shape, target: l.target })),
        distEnabled,
        distType,
        distAmount,
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
                if (typeof src.rate   === 'number') lfo[i].rate   = Math.max(0.05, Math.min(100, src.rate));
                if (typeof src.amount === 'number') lfo[i].amount = Math.max(0, Math.min(1, src.amount));
                if (typeof src.shape  === 'number') lfo[i].shape  = Math.max(0, Math.min(5, src.shape|0));
                if (typeof src.target === 'number') lfo[i].target = Math.max(0, Math.min(7, src.target|0));
            }
        }
        if (typeof s.distEnabled === 'boolean') distEnabled = s.distEnabled;
        if (typeof s.distType    === 'number')  distType    = Math.max(1, Math.min(3, s.distType|0));
        if (typeof s.distAmount  === 'number')  distAmount  = Math.max(0, Math.min(1, s.distAmount));
    } catch (e) {
        /* Corrupt or missing state file — use defaults */
    }
}

/* ── Value editing helpers ───────────────────────────────────────────────── */
function clampI(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
function clampF(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

function editLfoProp(dir) {
    const L = lfo[lfoTab];
    switch (lfoSel) {
        case 0: L.shape  = clampI(L.shape + dir, 0, LFO_SHAPES.length - 1); break;
        case 1: L.rate   = clampF(L.rate * (dir > 0 ? 1.12 : 0.893), 0.05, 100.0); break;
        case 2: L.amount = clampF(Math.round((L.amount + dir * 0.05) * 100) / 100, 0.0, 1.0); break;
        case 3: L.target = clampI(L.target + dir, 0, LFO_TARGETS.length - 1); break;
    }
    sendLfoParams(lfoTab);
    saveState();
    displayDirty = true;
}

function editDistProp(dir) {
    switch (distSel) {
        case 0: distType   = clampI(distType + dir, 1, 3); break;
        case 1: distAmount = clampF(Math.round((distAmount + dir * 0.05) * 100) / 100, 0.0, 1.0); break;
    }
    sendDistParams();
    saveState();
    displayDirty = true;
}

/* ── Input handling ─────────────────────────────────────────────────────── */
function handleJog(delta) {
    const dir = delta > 0 ? 1 : -1;

    if (state === STATE_KNOB_DISPLAY) return;

    if (state === STATE_MENU_MAIN) {
        mainSel = (mainSel + dir + 3) % 3;
        displayDirty = true;
        return;
    }

    if (state === STATE_MENU_LFO) {
        if (shiftHeld) {
            lfoTab     = (lfoTab + dir + 3) % 3;
            lfoEditing = false;
        } else if (lfoEditing) {
            editLfoProp(dir);
        } else {
            lfoSel = (lfoSel + dir + LFO_PROPS.length) % LFO_PROPS.length;
        }
        displayDirty = true;
        return;
    }

    if (state === STATE_MENU_DIST) {
        if (distEditing && distSel < 2) {
            editDistProp(dir);
        } else {
            distSel = (distSel + dir + 3) % 3;
        }
        displayDirty = true;
        return;
    }

    if (state === STATE_MENU_MODULE) {
        moduleSel = (moduleSel + dir + 2) % 2;
        displayDirty = true;
        return;
    }

    if (state === STATE_CONFIRM_UNLOAD) {
        confirmSel = (confirmSel + dir + 2) % 2;
        displayDirty = true;
    }
}

function handleClick() {
    if (state === STATE_KNOB_DISPLAY) return;

    if (state === STATE_MENU_MAIN) {
        if      (mainSel === 0) { state = STATE_MENU_LFO;    lfoSel = 0;   lfoEditing = false; }
        else if (mainSel === 1) { state = STATE_MENU_DIST;   distSel = 0;  distEditing = false; }
        else if (mainSel === 2) { state = STATE_MENU_MODULE; moduleSel = 0; }
        displayDirty = true;
        return;
    }

    if (state === STATE_MENU_LFO) {
        lfoEditing = !lfoEditing;
        displayDirty = true;
        return;
    }

    if (state === STATE_MENU_DIST) {
        if (distSel === 2) {
            distEnabled = !distEnabled;
            sendDistParams();
            saveState();
        } else {
            distEditing = !distEditing;
        }
        displayDirty = true;
        return;
    }

    if (state === STATE_MENU_MODULE) {
        if (moduleSel === 0) {
            /* SWAP — return to module picker (host unloads DSP and reloads menu) */
            host_return_to_menu();
        } else {
            /* UNLOAD — ask for confirmation first */
            confirmSel = 1;   /* default to NO */
            state      = STATE_CONFIRM_UNLOAD;
            displayDirty = true;
        }
        return;
    }

    if (state === STATE_CONFIRM_UNLOAD) {
        if (confirmSel === 0) {
            host_return_to_menu();  /* YES — unload and go to menu */
        } else {
            state = STATE_MENU_MODULE;  /* NO — back to module submenu */
            displayDirty = true;
        }
    }
}

function handleBack() {
    lfoEditing   = false;
    distEditing  = false;
    if (state === STATE_CONFIRM_UNLOAD) {
        state = STATE_MENU_MODULE;
        displayDirty = true;
    } else if (state !== STATE_MENU_MAIN && state !== STATE_KNOB_DISPLAY) {
        state = STATE_MENU_MAIN;
        displayDirty = true;
    }
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
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

/* Draw cursor for a list row.
 * Only call this for the currently selected row.
 * editing=true → filled block (you are changing the value)
 * editing=false → '>' arrow (you are choosing which row) */
function drawRowCursor(y, editing) {
    if (editing) {
        fill_rect(2, y, 5, 7, 1);
    } else {
        print(2, y, '>', 1);
    }
}

/* ── State screens ──────────────────────────────────────────────────────── */
function drawMenuMain() {
    clear_screen();
    drawTitle();
    drawSeparator(16);
    const items = ['LFO', 'DISTORTION', 'MODULE'];
    for (let i = 0; i < items.length; i++) {
        const y = 19 + i * 14;
        if (i === mainSel) print(2, y, '>', 1);
        print(12, y, items[i], 1);
    }
}

function formatHz(rate) {
    if (rate < 1)   return `${rate.toFixed(2)} Hz`;
    if (rate < 10)  return `${rate.toFixed(1)} Hz`;
    return `${Math.round(rate)} Hz`;
}

function drawMenuLfo() {
    clear_screen();
    /* Tab header */
    for (let t = 0; t < 3; t++) {
        const label = (t === lfoTab) ? `[LFO${t + 1}]` : ` LFO${t + 1} `;
        print(2 + t * 42, 1, label, 1);
    }
    drawSeparator(11);

    const L = lfo[lfoTab];
    const vals = [
        LFO_SHAPES[L.shape],
        formatHz(L.rate),
        L.amount.toFixed(2),
        LFO_TARGETS[L.target],
    ];

    for (let i = 0; i < LFO_PROPS.length; i++) {
        const y = 14 + i * 12;
        if (i === lfoSel) drawRowCursor(y, lfoEditing);
        print(12, y, `${LFO_PROPS[i]}: ${vals[i]}`, 1);
    }
}

function drawMenuDist() {
    clear_screen();
    print(2, 1, 'DISTORTION', 1);
    drawSeparator(11);

    const rows = [
        `TYPE:   ${DIST_TYPE_NAMES[distType - 1]}`,
        `AMOUNT: ${distAmount.toFixed(2)}`,
        distEnabled ? '[ ON  ]' : '[ OFF ]',
    ];

    for (let i = 0; i < rows.length; i++) {
        const y = 14 + i * 13;
        if (i === distSel) drawRowCursor(y, distEditing && i < 2);
        print(12, y, rows[i], 1);
    }
}

function drawMenuModule() {
    clear_screen();
    print(2, 1, 'MODULE', 1);
    drawSeparator(11);
    const items = ['SWAP MODULE', 'UNLOAD MODULE'];
    for (let i = 0; i < items.length; i++) {
        const y = 14 + i * 14;
        if (i === moduleSel) print(2, y, '>', 1);
        print(12, y, items[i], 1);
    }
}

function drawConfirmUnload() {
    clear_screen();
    print(2, 1, 'UNLOAD KWAHZOLIN?', 1);
    drawSeparator(11);
    const opts = ['YES', 'NO'];
    for (let i = 0; i < opts.length; i++) {
        const y = 16 + i * 14;
        if (i === confirmSel) print(2, y, '>', 1);
        print(12, y, opts[i], 1);
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
    switch (state) {
        case STATE_MENU_MAIN:      drawMenuMain();      break;
        case STATE_MENU_LFO:       drawMenuLfo();       break;
        case STATE_MENU_DIST:      drawMenuDist();      break;
        case STATE_MENU_MODULE:    drawMenuModule();    break;
        case STATE_KNOB_DISPLAY:   drawKnobDisplay();   break;
        case STATE_CONFIRM_UNLOAD: drawConfirmUnload(); break;
    }
}

/* ── Module lifecycle ────────────────────────────────────────────────────── */
globalThis.init = function () {
    /* Restore persisted LFO/distortion settings before sending to DSP */
    restoreState();

    /* Send all knob defaults to DSP */
    for (let i = 0; i < KNOB_COUNT; i++) sendKnobParam(i);
    /* Send restored (or default) LFO params to DSP */
    for (let i = 0; i < 3; i++) sendLfoParams(i);
    /* Send restored (or default) distortion params */
    sendDistParams();

    displayDirty = true;
};

globalThis.tick = function () {
    if (activeKnob >= 0) {
        knobTicks++;
        if (knobTicks > KNOB_TIMEOUT_TICKS) {
            activeKnob = -1;
            state = prevState;
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

    if (cc === CC_SHIFT) {
        shiftHeld = (val > 0);
        return;
    }

    /* Knobs 1-8 */
    if (cc >= KNOB_CC_BASE && cc < KNOB_CC_BASE + KNOB_COUNT) {
        const delta = decodeDelta(val);
        if (delta !== 0) {
            const idx = cc - KNOB_CC_BASE;
            knobValues[idx] = Math.max(0, Math.min(127, knobValues[idx] + delta));
            sendKnobParam(idx);
            activeKnob = idx;
            knobTicks  = 0;
            if (state !== STATE_KNOB_DISPLAY) prevState = state;
            state = STATE_KNOB_DISPLAY;
            displayDirty = true;
        }
        return;
    }

    /* Jog wheel */
    if (cc === CC_JOG_WHEEL) {
        const delta = decodeDelta(val);
        if (delta !== 0) handleJog(delta);
        return;
    }

    if (val === 0) return;

    if      (cc === CC_BACK)      handleBack();
    else if (cc === CC_JOG_CLICK) handleClick();
};
