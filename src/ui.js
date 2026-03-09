/*
 * DJ Deck - Dual-Deck Overtake Module UI
 * Copyright (c) 2026 DJ Hard Rich
 * Licensed under CC BY-NC-SA 4.0
 *
 * Turns Ableton Move into a dual-deck DJ controller with:
 *   - Two independent decks (A & B) on split 4x4 pad grids
 *   - Crossfader and per-deck volume controls
 *   - Knob pages (shift + data wheel to switch)
 *   - Per-deck hot cues, stutter, loop on pad pages
 *   - Shift + data push to load files to deck B
 *
 * Display: 128x64 1-bit OLED
 */

import * as os from 'os';

import { setButtonLED, setLED, clearAllLEDs, decodeDelta,
         shouldFilterMessage } from '/data/UserData/move-anything/shared/input_filter.mjs';
import { MoveBack, MovePlay, MoveShift, MoveRec,
         MoveMainKnob, MoveMainButton, MoveMaster,
         MoveRow1, MoveRow2, MoveRow3, MoveRow4,
         MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
         MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
         MoveKnob1Touch, MoveKnob2Touch, MoveKnob3Touch, MoveKnob4Touch,
         MoveKnob5Touch, MoveKnob6Touch, MoveKnob7Touch, MoveKnob8Touch,
         White, Black, BrightRed, BrightGreen, OrangeRed, Cyan,
         DarkGrey, WhiteLedDim, WhiteLedBright
         } from '/data/UserData/move-anything/shared/constants.mjs';
import { drawMenuHeader } from '/data/UserData/move-anything/shared/menu_layout.mjs';

/* ============================================================================
 * Constants
 * ============================================================================ */

let SCREEN_W = 128;
let SCREEN_H = 64;
let CHAR_W = 6;
let CHAR_H = 8;

/* Display rotation: 0=normal, 1=90CW, 2=180, 3=270CW */
let displayRotation = 0;

/* Pad pages (per-deck) */
let PAGE_HOT_CUE  = 0;
let PAGE_STUTTER  = 1;
let PAGE_LOOP     = 2;
let PAGE_CUE_EDIT = 3;

/* Knob pages */
let KNOB_PAGE_MAIN = 0;   /* pitch, speed, A vol, crossfader, B vol, deck sel, A vinyl, B vinyl */
let KNOB_PAGE_FX = 1;     /* pitch, speed, A filter, crossfader, B filter, deck sel, A vinyl, B vinyl */
let KNOB_PAGE_STEMS = 2;  /* A stem1-4, B stem1-4 */

/* Views */
let VIEW_DECK     = 0;
let VIEW_BROWSER  = 1;
let VIEW_PLAYLIST = 2;

/* Pad grid layout:
 * The 4x8 grid has notes 68-99.
 * Row 0 (bottom): 68-75, Row 1: 76-83, Row 2: 84-91, Row 3 (top): 92-99
 * Left 4 cols = Deck A, Right 4 cols = Deck B
 */
let PAD_BASE = 68;

/* Deck A pads: left 4 columns of each row */
let DECK_A_PADS = [
    68, 69, 70, 71,   /* row 0 left */
    76, 77, 78, 79,   /* row 1 left */
    84, 85, 86, 87,   /* row 2 left */
    92, 93, 94, 95    /* row 3 left */
];

/* Deck B pads: right 4 columns of each row */
let DECK_B_PADS = [
    72, 73, 74, 75,   /* row 0 right */
    80, 81, 82, 83,   /* row 1 right */
    88, 89, 90, 91,   /* row 2 right */
    96, 97, 98, 99    /* row 3 right */
];

/* Each deck uses bottom 2 rows (8 pads) for performance */
let NUM_PERF_PADS = 8;

/* Step notes */
let STEP_BASE = 16;

/* Track button CCs (reversed: CC43=Track1, CC40=Track4) */
let TRACK_CCS = [43, 42, 41, 40];

/* Stutter beat fractions */
let STUTTER_LABELS = ["1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4"];
let NUM_STUTTER = 8;

/* Loop beat sizes */
let LOOP_LABELS = ["1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1",
                    "2", "4", "8", "16", "32", "64"];
let NUM_LOOP = 13;

/* Hot loop (roll) sizes for top pads on cue page — maps pad offset (0-7) to loop index */
let HOT_LOOP_IDX = [1, 2, 3, 4, 5, 6, 7, 8]; /* 1/32, 1/16, 1/8, 1/4, 1/2, 1, 2, 4 */
let HOT_LOOP_LABELS = ["1/32", "1/16", "1/8", "1/4", "1/2", "1", "2", "4"];

/* Cue colors (bottom pads: edit/select) */
let CUE_COLORS = [BrightRed, 3, 7, 11, 14, 22, 25, 12];
/* Cue trigger colors (top pads on edit page: dimmer/pastel versions for triggering) */
let CUE_TRIGGER_COLORS = [9, 5, 8, 13, 16, 24, 27, 15];

/* Deck colors for pad dimming */
let DECK_A_COLOR = 33;   /* blue-ish */
let DECK_B_COLOR = OrangeRed;

/* LED init rate */
let LEDS_PER_FRAME = 8;

/* File browser root */
let BROWSER_ROOT = "/data/UserData/UserLibrary/Samples";

/* Supported extensions */
let WAV_EXTS = [".wav", ".mp3", ".m4a", ".aac", ".flac"];
let MOD_EXTS = [".mod", ".xm", ".it", ".s3m"];

/* Knob double-tap */
let DOUBLE_TAP_MS = 2100;

/* Slip mode button CCs */
let MoveCapture = 52;
let MoveSample = 118;
let MoveCopy = 60;

/* All knob touch notes */
let KNOB_TOUCHES = [0, 1, 2, 3, 4, 5, 6, 7]; /* MoveKnob1Touch..MoveKnob8Touch */

/* ============================================================================
 * Per-Deck State
 * ============================================================================ */

function makeDeckState() {
    return {
        playing: false,
        playPos: 0.0,
        totalFrames: 0,
        pitchSemitones: 0,
        speedPct: 100,
        bpm: 120.0,
        vinylSpeed: 100,
        stemVols: [100, 100, 100, 100],
        stemMutes: [0, 0, 0, 0],
        stemNames: ["", "", "", ""],
        stemLoaded: [0, 0, 0, 0],
        isMod: 0,
        cuePositions: [-1, -1, -1, -1, -1, -1, -1, -1],
        stutterActive: false,
        stutterPadHeld: -1,
        stutterPitch: 0,     /* -12 to +12 semitones */
        stutterFilter: 50,   /* 0=LPF, 50=bypass, 100=HPF */
        hotLoopHeld: -1,
        loopActive: false,
        loopSizeIdx: 6,
        /* currentPage is now global (shared across both decks) */
        filterPos: 50,
        editCueIdx: -1,
        waveform: null,
        slipMode: false,
        syncMode: false,
        detectedBpm: 120.0,
        beatgridBpm: 0.0,
        beatgridDownbeat: -1.0,
        wfZoomH: 1.0,   /* horizontal zoom 1x-16x */
        wfZoomV: 1.0,   /* vertical zoom 1x-4x */
        wfOffset: 0.0    /* horizontal scroll offset (0-1) */
    };
}

/* ============================================================================
 * Global State
 * ============================================================================ */

let deck = [makeDeckState(), makeDeckState()];
let activeDeck = 0; /* 0=A, 1=B - determines which deck knobs/tracks control */

let currentView = VIEW_DECK;
let knobPage = KNOB_PAGE_MAIN;
let currentPage = PAGE_HOT_CUE;  /* pad page — shared across both decks */

/* Mixer state */
let crossfader = 50;   /* 0=full A, 50=center, 100=full B */
let deckVol = [100, 100];
let masterVol = 100;

/* Knob double-tap reset */
let knobLastTouch = {};
let shiftHeld = false;
let needsRedraw = true;
let tickCount = 0;
let lastDrawTime = 0;
let MIN_DRAW_INTERVAL = 30; /* ms between redraws to reduce tearing */

/* End-of-track warning (30s) */
let WARN_SECONDS = 30;
let WARN_FLASH_MS = 500; /* toggle interval */
let warnFlashState = [false, false]; /* per-deck flash on/off */
let warnFlashTime = 0;

/* LED progressive init */
let ledInitPending = true;
let ledInitIndex = 0;

/* Input accumulation */
let pendingJogDelta = 0;

/* File browser */
let browserItems = [];
let browserSelectedIdx = 0;
let browserCurrentDir = BROWSER_ROOT;
let browserTargetStem = 0;
let browserTargetDeck = 0; /* which deck to load into */

/* Back button confirmation */
let backConfirmPending = false;
let backConfirmTime = 0;
let BACK_CONFIRM_MS = 1000;

/* Playlist browser */
let playlistNames = [];
let playlistTrackPaths = [];
let playlistSelectedIdx = 0;
let playlistBrowseMode = 0; /* 0=playlist list, 1=track list */
let playlistActiveIdx = -1; /* which playlist is open */
let playlistPageActive = false;

/* Parameter queue */
let paramQueue = [];

/* ============================================================================
 * Helpers
 * ============================================================================ */

function announce(text) {
    if (typeof host_send_screenreader === "function") {
        host_send_screenreader(text);
    } else if (typeof host_announce_screenreader === "function") {
        host_announce_screenreader(text);
    }
}

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

function truncStr(s, max) {
    if (!s) return "";
    return s.length <= max ? s : s.substring(0, max - 2) + "..";
}

/* Scrolling text state — resets when selected item changes */
let scrollTextLastItem = "";
let scrollTextOffset = 0;
let scrollTextPause = 0; /* ticks to pause at start/end */
let scrollTextActive = false; /* true when a long title is being scrolled */
let SCROLL_PAUSE_TICKS = 30; /* pause at each end (0°/180°) */
let SCROLL_PAUSE_TICKS_90 = 15; /* shorter pause for 90°/270° (50% of normal) */
let SCROLL_SPEED_TICKS = 3;  /* ticks between each scroll step (0°/180°) */
let SCROLL_SPEED_TICKS_90 = 1; /* faster scroll for 90°/270° views (~2.2x by also advancing 2 chars sometimes) */
let scroll90Counter = 0; /* fractional accumulator for 2.2x speed in 90° */

function scrollStr(s, maxChars) {
    if (!s) { scrollTextActive = false; return ""; }
    if (s.length <= maxChars) { scrollTextActive = false; return s; }

    scrollTextActive = true;

    /* Reset scroll when selected item changes */
    let isRotated90 = (displayRotation === 1 || displayRotation === 3);
    let pauseTicks = isRotated90 ? SCROLL_PAUSE_TICKS_90 : SCROLL_PAUSE_TICKS;
    if (s !== scrollTextLastItem) {
        scrollTextLastItem = s;
        scrollTextOffset = 0;
        scrollTextPause = pauseTicks;
    }

    let maxOffset = s.length - maxChars;
    let displayStr = s.substring(scrollTextOffset, scrollTextOffset + maxChars);

    /* Advance scroll on tick — 2.2x faster in 90°/270° rotated views */
    if (scrollTextPause > 0) {
        scrollTextPause--;
    } else if (isRotated90) {
        /* 2.2x speed: advance every tick, and every 5th tick advance an extra char.
         * (1 + 1/5 = 1.2 chars/tick vs 1/3 chars/tick normal = ~3.6x per tick,
         *  but normal is every 3 ticks so effective ratio = 1.2 / (1/3) ≈ 2.2x ×  */
        scroll90Counter += 22; /* accumulate 2.2 per tick (scaled by 10) */
        let steps = Math.floor(scroll90Counter / 30); /* one step per 30/22 ≈ 1.36 ticks */
        scroll90Counter %= 30;
        for (let i = 0; i < steps; i++) {
            scrollTextOffset++;
            if (scrollTextOffset > maxOffset) {
                scrollTextOffset = 0;
                scrollTextPause = pauseTicks;
                scroll90Counter = 0;
                break;
            } else if (scrollTextOffset === maxOffset) {
                scrollTextPause = pauseTicks;
                scroll90Counter = 0;
                break;
            }
        }
    } else if (tickCount % SCROLL_SPEED_TICKS === 0) {
        scrollTextOffset++;
        if (scrollTextOffset > maxOffset) {
            scrollTextOffset = 0;
            scrollTextPause = SCROLL_PAUSE_TICKS;
        } else if (scrollTextOffset === maxOffset) {
            scrollTextPause = SCROLL_PAUSE_TICKS;
        }
    }

    return displayStr;
}

function pathBasename(path) {
    let idx = path.lastIndexOf("/");
    return idx >= 0 ? path.slice(idx + 1) : path;
}

function pathDirname(path) {
    let idx = path.lastIndexOf("/");
    if (idx <= 0) return "/";
    return path.slice(0, idx);
}

function hasExtension(name, exts) {
    let lower = name.toLowerCase();
    for (let i = 0; i < exts.length; i++) {
        if (lower.endsWith(exts[i])) return true;
    }
    return false;
}

function isDirectory(path) {
    try {
        let st = os.stat(path);
        if (Array.isArray(st)) {
            let obj = st[0];
            if (obj && typeof obj.mode === "number") {
                return (obj.mode & 0o170000) === 0o040000;
            }
        }
        return false;
    } catch (e) { return false; }
}

/* Deck param prefix: "a_" or "b_" */
function dp(deckIdx, key) {
    return (deckIdx === 0 ? "a_" : "b_") + key;
}

/* 4x5 pixel font for rotated text rendering */
let PFONT = {
    '0':[6,9,9,9,6],'1':[2,6,2,2,7],'2':[6,9,2,4,15],'3':[6,9,2,9,6],
    '4':[9,9,15,1,1],'5':[15,8,14,1,14],'6':[6,8,14,9,6],'7':[15,1,2,4,4],
    '8':[6,9,6,9,6],'9':[6,9,7,1,6],
    'A':[6,9,15,9,9],'B':[14,9,14,9,14],'C':[6,9,8,9,6],'D':[14,9,9,9,14],
    'E':[15,8,14,8,15],'F':[15,8,14,8,8],'G':[6,9,8,11,6],'H':[9,9,15,9,9],
    'I':[7,2,2,2,7],'J':[1,1,1,9,6],'K':[9,10,12,10,9],'L':[8,8,8,8,15],
    'M':[9,15,15,9,9],'N':[9,13,15,11,9],'O':[6,9,9,9,6],'P':[14,9,14,8,8],
    'R':[14,9,14,10,9],'S':[7,8,6,1,14],'T':[15,2,2,2,2],'U':[9,9,9,9,6],
    'V':[9,9,9,6,6],'W':[9,9,15,15,9],'X':[9,9,6,9,9],'Y':[9,9,7,1,6],
    'Z':[15,1,2,4,15],
    'b':[8,8,14,9,14],'s':[0,7,12,3,14],'t':[4,14,4,4,3],'x':[0,9,6,9,0],
    ':':[0,2,0,2,0],'/':[1,2,2,4,8],'%':[9,1,2,4,9],'+':[0,2,7,2,0],
    '-':[0,0,15,0,0],'.':[0,0,0,0,2],'>':[4,2,1,2,4],'|':[2,2,2,2,2],
    '[':[6,4,4,4,6],']':[6,2,2,2,6],'!':[2,2,2,0,2],' ':[0,0,0,0,0]
};

/* Transform user coords to screen coords based on rotation.
 * User coords: (ux, uy) in a virtual canvas that matches the user's view.
 *   rot 0: 128x64 (normal), rot 1: 64x128 (90CW), rot 2: 128x64 (180), rot 3: 64x128 (270) */
function toScreen(ux, uy) {
    if (displayRotation === 0) return [ux, uy];
    if (displayRotation === 2) return [SCREEN_W - 1 - ux, SCREEN_H - 1 - uy];
    if (displayRotation === 1) return [uy, SCREEN_H - 1 - ux]; /* 90 CW */
    return [SCREEN_W - 1 - uy, ux]; /* 270 CW */
}

/* Draw a single pixel in user coords */
function rPixel(ux, uy, color) {
    if (displayRotation === 1) { set_pixel(uy, SCREEN_H - 1 - ux, color); return; }
    if (displayRotation === 3) { set_pixel(SCREEN_W - 1 - uy, ux, color); return; }
    if (displayRotation === 2) { set_pixel(SCREEN_W - 1 - ux, SCREEN_H - 1 - uy, color); return; }
    set_pixel(ux, uy, color);
}

/* Draw a single character using pixel font at user coords */
function drawPixelChar(ux, uy, ch, color) {
    let glyph = PFONT[ch] || PFONT[' '];
    for (let row = 0; row < 5; row++) {
        let bits = glyph[row];
        for (let col = 0; col < 4; col++) {
            if (bits & (8 >> col)) {
                rPixel(ux + col, uy + row, color);
            }
        }
    }
}

/* Draw text string using pixel font at user coords */
function drawPixelText(ux, uy, text, color) {
    for (let i = 0; i < text.length; i++) {
        drawPixelChar(ux + i * 5, uy, text.charAt(i), color);
    }
}

/* Rotated print: uses native print for 0/180, pixel font for 90/270 */
function rPrint(x, y, text, color) {
    if (displayRotation === 0) { print(x, y, text, color); return; }
    if (displayRotation === 2) {
        let tw = text.length * CHAR_W;
        print(SCREEN_W - x - tw, SCREEN_H - y - CHAR_H, text, color);
        return;
    }
    /* 90/270: use pixel font */
    drawPixelText(x, y, String(text).toUpperCase(), color);
}

/* Rotated fill_rect */
function rFillRect(x, y, w, h, color) {
    if (displayRotation === 0) { fill_rect(x, y, w, h, color); return; }
    if (displayRotation === 2) {
        fill_rect(SCREEN_W - x - w, SCREEN_H - y - h, w, h, color);
        return;
    }
    /* 90/270: use transformed fill_rect (faster & avoids tearing) */
    if (displayRotation === 1) {
        fill_rect(y, SCREEN_H - x - w, h, w, color);
    } else {
        fill_rect(SCREEN_W - y - h, x, h, w, color);
    }
}

/* Rotated draw_rect (outline) */
function rDrawRect(x, y, w, h, color) {
    if (displayRotation === 0) { draw_rect(x, y, w, h, color); return; }
    if (displayRotation === 2) {
        draw_rect(SCREEN_W - x - w, SCREEN_H - y - h, w, h, color);
        return;
    }
    /* 90/270: transform the rect */
    if (displayRotation === 1) {
        draw_rect(y, SCREEN_H - x - w, h, w, color);
    } else {
        draw_rect(SCREEN_W - y - h, x, h, w, color);
    }
}

/* Virtual canvas dimensions for current rotation */
function canvasW() { return (displayRotation === 1 || displayRotation === 3) ? SCREEN_H : SCREEN_W; }
function canvasH() { return (displayRotation === 1 || displayRotation === 3) ? SCREEN_W : SCREEN_H; }

/* === Local overlay system (rotation-aware) === */
let overlayName = "";
let overlayValue = "";
let overlayExpire = 0;
let OVERLAY_DURATION = 400;

function showOverlay(name, value, duration) {
    overlayName = String(name);
    overlayValue = String(value);
    overlayExpire = Date.now() + (duration || OVERLAY_DURATION);
    needsRedraw = true;
}

function tickOverlay() {
    if (overlayExpire > 0 && Date.now() >= overlayExpire) {
        overlayExpire = 0;
        overlayName = "";
        overlayValue = "";
        backConfirmPending = false;
        return true;
    }
    return false;
}

function dismissOverlayOnInput() {
    if (overlayExpire > 0) {
        overlayExpire = 0;
        overlayName = "";
        overlayValue = "";
        needsRedraw = true;
    }
}

function rDrawOverlay() {
    if (!overlayName) return;
    let cw = canvasW();
    let ch = canvasH();
    let isVertical = (displayRotation === 1 || displayRotation === 3);
    let boxW, boxH, boxX, boxY;
    if (isVertical) {
        boxW = cw - 8;
        boxH = 20;
        boxX = 4;
        boxY = Math.floor((ch - boxH) / 2);
        rFillRect(boxX, boxY, boxW, boxH, 0);
        rDrawRect(boxX, boxY, boxW, boxH, 1);
        rPrint(boxX + 3, boxY + 3, overlayName, 1);
        rPrint(boxX + 3, boxY + 11, overlayValue, 1);
    } else {
        boxW = 80;
        boxH = 24;
        boxX = Math.floor((cw - boxW) / 2);
        boxY = Math.floor((ch - boxH) / 2);
        rFillRect(boxX, boxY, boxW, boxH, 0);
        rDrawRect(boxX, boxY, boxW, boxH, 1);
        rPrint(boxX + 4, boxY + 2, overlayName, 1);
        rPrint(boxX + 4, boxY + 14, overlayValue, 1);
    }
}

/* Get knob label for current page. knobIdx 0-7 for knobs 1-8 */
function getKnobLabel(knobIdx) {
    let dl = activeDeck === 0 ? "A" : "B";
    if (currentPage === PAGE_STUTTER) {
        let labels = [
            dl + " Stt Pitch", dl + " Stt Filter",
            "Deck A Vol", "Crossfader", "Deck B Vol",
            "Deck Select", "A Vinyl Spd", "B Vinyl Spd"
        ];
        return labels[knobIdx] || "";
    }
    if (currentPage === PAGE_CUE_EDIT) {
        let labels = [
            "Cue Coarse", "Cue Fine",
            "H Zoom", "V Zoom", "Scroll",
            "Deck Select", "", ""
        ];
        return labels[knobIdx] || "";
    }
    if (knobPage === KNOB_PAGE_MAIN) {
        let labels = [
            dl + " Pitch", dl + " Speed",
            "Deck A Vol", "Crossfader", "Deck B Vol",
            "Deck Select", "A Vinyl Spd", "B Vinyl Spd"
        ];
        return labels[knobIdx] || "";
    } else if (knobPage === KNOB_PAGE_FX) {
        let labels = [
            dl + " Pitch", dl + " Speed",
            "A DJ Filter", "Crossfader", "B DJ Filter",
            "Deck Select", "A Vinyl Spd", "B Vinyl Spd"
        ];
        return labels[knobIdx] || "";
    } else {
        let labels = [
            "A Stem 1 Vol", "A Stem 2 Vol", "A Stem 3 Vol", "A Stem 4 Vol",
            "B Stem 1 Vol", "B Stem 2 Vol", "B Stem 3 Vol", "B Stem 4 Vol"
        ];
        return labels[knobIdx] || "";
    }
}

function queueParam(key, val) {
    paramQueue.push([key, val]);
}

function sendParamNow(key, val) {
    host_module_set_param(key, val);
}

function performSync(d) {
    /* Sync deck d's vinyl speed to match the other deck's effective BPM.
     * Uses beatgrid_bpm (exact, to 0.01) when available, falls back to detected. */
    let other = d === 0 ? 1 : 0;
    let dk = deck[d];
    let otherDk = deck[other];
    let baseBpm = dk.beatgridBpm > 0 ? dk.beatgridBpm : dk.detectedBpm;
    if (baseBpm <= 0 || otherDk.bpm <= 0) return;
    /* Target: baseBpm * (speed/100) * (newVinyl/100) = otherDk.bpm */
    let speed = dk.speedPct / 100.0;
    if (speed <= 0) speed = 1.0;
    /* Use float vinyl (not rounded) for hundredth-of-BPM precision,
     * then round to nearest integer for the vinyl_speed param */
    let exactVinyl = otherDk.bpm * 100.0 / (baseBpm * speed);
    let newVinyl = clamp(Math.round(exactVinyl), 50, 150);
    dk.vinylSpeed = newVinyl;
    sendParamNow(dp(d, "vinyl_speed"), String(newVinyl));
}

/* Map a pad note to {deck, padIdx} or null */
function padToDeck(note) {
    for (let i = 0; i < 16; i++) {
        if (DECK_A_PADS[i] === note) return { deck: 0, padIdx: i };
        if (DECK_B_PADS[i] === note) return { deck: 1, padIdx: i };
    }
    return null;
}

/* ============================================================================
 * DSP Communication
 * ============================================================================ */

function syncDeckFromDsp(d) {
    let prefix = d === 0 ? "a_" : "b_";
    let dk = deck[d];
    let v;
    v = host_module_get_param(prefix + "playing");
    if (v) dk.playing = (v === "1");
    v = host_module_get_param(prefix + "play_pos");
    if (v) dk.playPos = parseFloat(v);
    v = host_module_get_param(prefix + "total_frames");
    if (v) dk.totalFrames = parseInt(v, 10);
    v = host_module_get_param(prefix + "is_mod");
    if (v) dk.isMod = parseInt(v, 10);
    v = host_module_get_param(prefix + "bpm");
    if (v) dk.bpm = parseFloat(v);
    v = host_module_get_param(prefix + "detected_bpm");
    if (v) dk.detectedBpm = parseFloat(v);
    v = host_module_get_param(prefix + "vinyl_speed");
    if (v) dk.vinylSpeed = parseInt(v, 10);
    v = host_module_get_param(prefix + "filter");
    if (v) dk.filterPos = parseInt(v, 10);
    v = host_module_get_param(prefix + "beatgrid_bpm");
    if (v) dk.beatgridBpm = parseFloat(v);
    v = host_module_get_param(prefix + "beatgrid_downbeat");
    if (v) dk.beatgridDownbeat = parseFloat(v);
}

function syncMixerFromDsp() {
    let v;
    v = host_module_get_param("crossfader");
    if (v) crossfader = parseInt(v, 10);
    v = host_module_get_param("a_vol");
    if (v) deckVol[0] = parseInt(v, 10);
    v = host_module_get_param("b_vol");
    if (v) deckVol[1] = parseInt(v, 10);
    v = host_module_get_param("master_vol");
    if (v) masterVol = parseInt(v, 10);
}

function syncStemInfo(d) {
    let prefix = d === 0 ? "a_" : "b_";
    let dk = deck[d];
    for (let t = 0; t < 4; t++) {
        let v = host_module_get_param(prefix + "stem_name_" + t);
        if (v) dk.stemNames[t] = v;
        v = host_module_get_param(prefix + "stem_loaded_" + t);
        if (v) dk.stemLoaded[t] = parseInt(v, 10);
    }
}

function syncCuePositions(d) {
    let prefix = d === 0 ? "a_" : "b_";
    let dk = deck[d];
    for (let c = 0; c < 8; c++) {
        let v = host_module_get_param(prefix + "cue_pos_" + c);
        if (v) dk.cuePositions[c] = parseFloat(v);
    }
}

function syncWaveform(d) {
    let prefix = d === 0 ? "a_" : "b_";
    let v = host_module_get_param(prefix + "waveform");
    if (v && v.length === 128) {
        let wf = [];
        for (let i = 0; i < 128; i++) {
            let ch = v.charCodeAt(i);
            if (ch >= 48 && ch <= 57) wf.push(ch - 48);       /* 0-9 */
            else if (ch >= 97 && ch <= 102) wf.push(ch - 87); /* a-f */
            else wf.push(0);
        }
        deck[d].waveform = wf;
    }
}

/* ============================================================================
 * File Browser
 * ============================================================================ */

function openBrowser(targetDeck) {
    browserTargetDeck = targetDeck;
    browserCurrentDir = BROWSER_ROOT;
    browserSelectedIdx = 0;
    browserTargetStem = 0;
    refreshBrowser();
    currentView = VIEW_BROWSER;
    announce("File browser, Deck " + (targetDeck === 0 ? "A" : "B"));
}

function refreshBrowser() {
    browserItems = [];

    if (browserCurrentDir !== BROWSER_ROOT) {
        browserItems.push({ kind: "up", label: "..", path: pathDirname(browserCurrentDir) });
    }

    try {
        let raw = os.readdir(browserCurrentDir) || [];
        let names = [];
        if (Array.isArray(raw) && Array.isArray(raw[0])) {
            names = raw[0];
        } else if (Array.isArray(raw)) {
            names = raw;
        }

        let dirs = [];
        let files = [];
        for (let i = 0; i < names.length; i++) {
            let name = names[i];
            if (!name || name === "." || name === ".." || name.startsWith(".")) continue;
            let fullPath = browserCurrentDir + "/" + name;
            if (isDirectory(fullPath)) {
                dirs.push({ kind: "dir", label: "[" + name + "]", path: fullPath });
            } else if (hasExtension(name, WAV_EXTS) || hasExtension(name, MOD_EXTS)) {
                files.push({ kind: "file", label: name, path: fullPath });
            }
        }
        dirs.sort(function(a, b) { return a.label.localeCompare(b.label); });
        files.sort(function(a, b) { return a.label.localeCompare(b.label); });

        for (let j = 0; j < dirs.length; j++) browserItems.push(dirs[j]);
        for (let k = 0; k < files.length; k++) browserItems.push(files[k]);
    } catch (e) {
        browserItems.push({ kind: "error", label: "(cannot read)", path: "" });
    }

    if (browserSelectedIdx >= browserItems.length) {
        browserSelectedIdx = Math.max(0, browserItems.length - 1);
    }
}

function browserNavigate(delta) {
    if (browserItems.length === 0) return;
    browserSelectedIdx = clamp(browserSelectedIdx + (delta > 0 ? 1 : -1),
                               0, browserItems.length - 1);
    let sel = browserItems[browserSelectedIdx];
    if (sel) announce(sel.label);
}

function browserSelect() {
    if (browserItems.length === 0) return;
    let sel = browserItems[browserSelectedIdx];
    if (!sel) return;

    if (sel.kind === "up" || sel.kind === "dir") {
        browserCurrentDir = sel.path;
        browserSelectedIdx = 0;
        refreshBrowser();
    } else if (sel.kind === "file") {
        let prefix = browserTargetDeck === 0 ? "a_" : "b_";
        let deckLabel = browserTargetDeck === 0 ? "A" : "B";
        deckVol[browserTargetDeck] = 100;
        sendParamNow(prefix + "vol", "100");
        if (hasExtension(sel.label, MOD_EXTS)) {
            queueParam(prefix + "load_file", sel.path);
            announce("Loading " + sel.label + " to Deck " + deckLabel);
            showOverlay("Deck " + deckLabel + ": MOD", sel.label);
        } else {
            queueParam(prefix + "stem_path_" + browserTargetStem, sel.path);
            announce("Loading " + sel.label + " to Deck " + deckLabel);
            showOverlay("Deck " + deckLabel + " S" + (browserTargetStem + 1), sel.label);
        }
        currentView = VIEW_DECK;
    }
}

function browserBack() {
    if (browserCurrentDir !== BROWSER_ROOT) {
        let parent = pathDirname(browserCurrentDir);
        if (parent.length < BROWSER_ROOT.length) parent = BROWSER_ROOT;
        browserCurrentDir = parent;
        browserSelectedIdx = 0;
        refreshBrowser();
    } else {
        currentView = VIEW_DECK;
    }
}

/* ============================================================================
 * Playlist Browser
 * ============================================================================ */

function openPlaylistView() {
    playlistPageActive = true;
    playlistBrowseMode = 0;
    playlistSelectedIdx = 0;
    playlistActiveIdx = -1;
    playlistNames = [];
    playlistTrackPaths = [];

    /* Trigger a scan and load playlist names */
    let countStr = host_module_get_param("playlist_scan");
    let count = countStr ? parseInt(countStr) : 0;
    for (let i = 0; i < count; i++) {
        let name = host_module_get_param("playlist_name_" + i);
        if (name) playlistNames.push(name);
    }
    currentView = VIEW_PLAYLIST;
}

function playlistNavigate(delta) {
    if (playlistBrowseMode === 0) {
        if (playlistNames.length === 0) return;
        playlistSelectedIdx = clamp(playlistSelectedIdx + (delta > 0 ? 1 : -1),
                                    0, playlistNames.length - 1);
        announce(playlistNames[playlistSelectedIdx]);
    } else {
        if (playlistTrackPaths.length === 0) return;
        playlistSelectedIdx = clamp(playlistSelectedIdx + (delta > 0 ? 1 : -1),
                                    0, playlistTrackPaths.length - 1);
        announce(pathBasename(playlistTrackPaths[playlistSelectedIdx]));
    }
}

function playlistSelect(targetDeck) {
    if (playlistBrowseMode === 0) {
        /* Enter playlist: load track list */
        if (playlistNames.length === 0) return;
        playlistActiveIdx = playlistSelectedIdx;
        playlistTrackPaths = [];
        let countStr = host_module_get_param("playlist_track_count_" + playlistActiveIdx);
        let count = countStr ? parseInt(countStr) : 0;
        for (let i = 0; i < count; i++) {
            let path = host_module_get_param("playlist_track_" + playlistActiveIdx + "_" + i);
            if (path) playlistTrackPaths.push(path);
        }
        playlistSelectedIdx = 0;
        playlistBrowseMode = 1;
        announce("Playlist " + playlistNames[playlistActiveIdx]);
    } else {
        /* Load selected track to target deck */
        if (playlistTrackPaths.length === 0) return;
        let path = playlistTrackPaths[playlistSelectedIdx];
        let prefix = targetDeck === 0 ? "a_" : "b_";
        let deckLabel = targetDeck === 0 ? "A" : "B";
        deckVol[targetDeck] = 100;
        sendParamNow(prefix + "vol", "100");
        if (hasExtension(pathBasename(path), MOD_EXTS)) {
            queueParam(prefix + "load_file", path);
        } else {
            queueParam(prefix + "stem_path_0", path);
        }
        announce("Loading " + pathBasename(path) + " to Deck " + deckLabel);
        showOverlay("Deck " + deckLabel, pathBasename(path));
        currentView = VIEW_DECK;
        playlistPageActive = false;
    }
}

function playlistBack() {
    if (playlistBrowseMode === 1) {
        playlistBrowseMode = 0;
        playlistSelectedIdx = playlistActiveIdx;
        playlistActiveIdx = -1;
        playlistTrackPaths = [];
        announce("Playlists");
    } else {
        currentView = VIEW_DECK;
        playlistPageActive = false;
        announce("Deck view");
    }
}

function drawPlaylist() {
    clear_screen();

    let title;
    let items;
    if (playlistBrowseMode === 0) {
        title = "Playlists";
        items = playlistNames;
    } else {
        title = playlistNames[playlistActiveIdx] || "Playlist";
        items = playlistTrackPaths.map(function(p) { return pathBasename(p); });
    }

    drawMenuHeader(truncStr(title, 18));

    let listY = 14;
    let lineH = 9;
    let maxVisible = Math.floor((55 - listY) / lineH);
    let startIdx = 0;
    if (playlistSelectedIdx > maxVisible - 2) startIdx = playlistSelectedIdx - (maxVisible - 2);
    let endIdx = Math.min(startIdx + maxVisible, items.length);

    for (let i = startIdx; i < endIdx; i++) {
        let y = listY + (i - startIdx) * lineH;
        let isSel = (i === playlistSelectedIdx);
        let label = isSel ? scrollStr(items[i], 20) : truncStr(items[i], 20);

        if (isSel) {
            rFillRect(0, y - 1, SCREEN_W, lineH, 1);
            rPrint(4, y, label, 0);
        } else {
            rPrint(4, y, label, 1);
        }
    }

    if (items.length === 0) {
        rPrint(4, listY, "(empty)", 1);
    }

    rFillRect(0, 55, SCREEN_W, 1, 1);
    if (playlistBrowseMode === 1) {
        rPrint(2, 57, "Push:Load Shf:DkB", 1);
    } else {
        rPrint(2, 57, "Push:Open  Back:Exit", 1);
    }
}

/* ============================================================================
 * LED Management
 * ============================================================================ */

function setupLedBatch() {
    let total = 32 + 16 + 8;
    let end = Math.min(ledInitIndex + LEDS_PER_FRAME, total);

    for (let i = ledInitIndex; i < end; i++) {
        if (i < 32) {
            setLED(PAD_BASE + i, DarkGrey, true);
        } else if (i < 48) {
            setLED(STEP_BASE + (i - 32), Black, true);
        }
    }
    ledInitIndex = end;
    if (ledInitIndex >= total) {
        ledInitPending = false;
        ledInitIndex = 0;
        updateAllLEDs();
    }
}

function updateAllLEDs() {
    updatePageLEDs();
    updatePadLEDs();
    updateTrackLEDs();
    updateControlLEDs();
}

function updatePageLEDs() {
    /* Steps 1-4 show pad page (shared) */
    for (let i = 0; i < 4; i++) {
        setLED(STEP_BASE + i, (i === currentPage) ? White : DarkGrey, true);
    }
    /* Steps 5-7 show knob page */
    setLED(STEP_BASE + 4, (knobPage === KNOB_PAGE_MAIN) ? BrightGreen : DarkGrey, true);
    setLED(STEP_BASE + 5, (knobPage === KNOB_PAGE_FX) ? OrangeRed : DarkGrey, true);
    setLED(STEP_BASE + 6, (knobPage === KNOB_PAGE_STEMS) ? Cyan : DarkGrey, true);
    /* Step 8: active deck indicator */
    setLED(STEP_BASE + 7, (activeDeck === 0) ? DECK_A_COLOR : DECK_B_COLOR, true);
    /* Step 9: Deck A slip, Step 10: Deck B slip */
    setLED(STEP_BASE + 8, deck[0].slipMode ? BrightGreen : DarkGrey, true);
    setLED(STEP_BASE + 9, deck[1].slipMode ? BrightGreen : DarkGrey, true);
    /* Step 11: empty gap */
    setLED(STEP_BASE + 10, Black, true);
    /* Step 12: Deck A sync, Step 13: Deck B sync */
    setLED(STEP_BASE + 11, deck[0].syncMode ? OrangeRed : DarkGrey, true);
    setLED(STEP_BASE + 12, deck[1].syncMode ? OrangeRed : DarkGrey, true);
    /* Steps 14-15 dark, Step 16 = playlist */
    for (let j = 13; j < 15; j++) {
        setLED(STEP_BASE + j, Black, true);
    }
    setLED(STEP_BASE + 15, playlistPageActive ? 25 : DarkGrey, true); /* 25 = purple */
}

function updatePadLEDs() {
    /* Update both deck pad grids */
    for (let d = 0; d < 2; d++) {
        let pads = d === 0 ? DECK_A_PADS : DECK_B_PADS;
        let dk = deck[d];
        let dimColor = d === 0 ? DECK_A_COLOR : DECK_B_COLOR;

        if (currentPage === PAGE_LOOP) {
            /* Loop page uses all 16 pads for 14 loop sizes */
            for (let i = 0; i < 16; i++) {
                if (i < NUM_LOOP) {
                    let isSelected = (dk.loopActive && i === dk.loopSizeIdx);
                    setLED(pads[i], isSelected ? BrightGreen : DarkGrey, true);
                } else {
                    setLED(pads[i], Black, true);
                }
            }
        } else if (currentPage === PAGE_CUE_EDIT) {
            /* Cue edit: top pads = cue TRIGGERS (different colors), bottom = cue select/edit */
            for (let i = 0; i < NUM_PERF_PADS; i++) {
                /* Top pads (8-15): trigger colors — lit if cue exists */
                let trigColor = (dk.cuePositions[i] >= 0) ? CUE_TRIGGER_COLORS[i] : Black;
                setLED(pads[i + 8], trigColor, true);
                /* Bottom pads (0-7): edit colors */
                if (dk.editCueIdx === i) {
                    setLED(pads[i], White, true);
                } else {
                    let color = (dk.cuePositions[i] >= 0) ? CUE_COLORS[i] : DarkGrey;
                    setLED(pads[i], color, true);
                }
            }
        } else {
            /* Top 2 rows (indices 8-15) */
            if (currentPage === PAGE_HOT_CUE) {
                /* Hot loop pads */
                for (let p = 0; p < 8; p++) {
                    let isHeld = (dk.hotLoopHeld === p);
                    setLED(pads[p + 8], isHeld ? White : DarkGrey, true);
                }
            } else if (currentPage === PAGE_STUTTER) {
                /* Top pads = stutter sizes (hold to stutter) */
                for (let p = 0; p < NUM_STUTTER; p++) {
                    let isHeld = (dk.stutterPadHeld === p);
                    setLED(pads[p + 8], isHeld ? White : OrangeRed, true);
                }
            } else {
                for (let p = 8; p < 16; p++) {
                    setLED(pads[p], Black, true);
                }
            }

            /* Bottom 2 rows (indices 0-7): performance pads */
            if (currentPage === PAGE_HOT_CUE) {
                for (let i = 0; i < NUM_PERF_PADS; i++) {
                    let color = (dk.cuePositions[i] >= 0) ? CUE_COLORS[i] : Black;
                    setLED(pads[i], color, true);
                }
            } else if (currentPage === PAGE_STUTTER) {
                /* Bottom pads = hotcue triggers (so you can jump while stuttering) */
                for (let i = 0; i < NUM_PERF_PADS; i++) {
                    let color = (dk.cuePositions[i] >= 0) ? CUE_COLORS[i] : DarkGrey;
                    setLED(pads[i], color, true);
                }
            }
        }
    }
}

function updateTrackLEDs() {
    let dk = deck[activeDeck];
    for (let t = 0; t < 4; t++) {
        let cc = TRACK_CCS[t];
        if (!dk.stemLoaded[t]) {
            setButtonLED(cc, Black);
        } else if (dk.stemMutes[t]) {
            setButtonLED(cc, WhiteLedBright);
        } else {
            setButtonLED(cc, WhiteLedDim);
        }
    }
}

function updateControlLEDs() {
    /* Play = Deck A, Rec = Deck B */
    setButtonLED(MovePlay, deck[0].playing ? WhiteLedBright : Black);
    setButtonLED(MoveRec, deck[1].playing ? WhiteLedBright : Black);
    setButtonLED(MoveBack, WhiteLedBright);
    setButtonLED(MoveCopy, WhiteLedDim);
}

/* ============================================================================
 * Drawing
 * ============================================================================ */

function drawDeck() {
    clear_screen();

    /* === Header (y=0): Deck A info | Deck B info === */
    let dkA = deck[0];
    let dkB = deck[1];

    /* Deck A side (left half) */
    let aPlay = dkA.playing ? ">" : "||";
    rPrint(1, 0, aPlay, 1);
    rPrint(14, 0, dkA.bpm.toFixed(1) + "BPM", 1);
    /* Active deck indicator */
    if (activeDeck === 0) rFillRect(0, 7, 63, 1, 1);

    /* Divider */
    rFillRect(63, 0, 1, 54, 1);

    /* Deck B side (right half) */
    let bPlay = dkB.playing ? ">" : "||";
    rPrint(65, 0, bPlay, 1);
    rPrint(78, 0, dkB.bpm.toFixed(1) + "BPM", 1);
    if (activeDeck === 1) rFillRect(64, 7, 63, 1, 1);

    /* === Row 2 (y=9): Pitch/speed info per deck === */
    let aPitch = dkA.pitchSemitones >= 0 ? "+" + dkA.pitchSemitones : String(dkA.pitchSemitones);
    rPrint(1, 9, aPitch + "st " + dkA.speedPct + "%", 1);

    let bPitch = dkB.pitchSemitones >= 0 ? "+" + dkB.pitchSemitones : String(dkB.pitchSemitones);
    rPrint(65, 9, bPitch + "st " + dkB.speedPct + "%", 1);

    /* === Row 3 (y=17): Position bars === */
    /* Deck A position bar */
    rDrawRect(0, 17, 62, 5, 1);
    if (dkA.totalFrames > 0) {
        let posXA = Math.floor(dkA.playPos * 60);
        rFillRect(1, 18, clamp(posXA, 1, 60), 3, 1);
    }

    /* Deck B position bar */
    rDrawRect(65, 17, 62, 5, 1);
    if (dkB.totalFrames > 0) {
        let posXB = Math.floor(dkB.playPos * 60);
        rFillRect(66, 18, clamp(posXB, 1, 60), 3, 1);
    }

    /* Skip rows 4-5 on cue edit page — waveform uses that space */
    let cueEditActive = currentPage === PAGE_CUE_EDIT;

    /* === Row 4 (y=23): Stems status (compact) === */
    if (!cueEditActive) {
    for (let t = 0; t < 4; t++) {
        /* Deck A stems */
        let ax = t * 15 + 1;
        if (dkA.stemLoaded[t]) {
            rPrint(ax, 23, dkA.stemMutes[t] ? "x" : String(t + 1), 1);
        }
        /* Deck B stems */
        let bx = 65 + t * 15;
        if (dkB.stemLoaded[t]) {
            rPrint(bx, 23, dkB.stemMutes[t] ? "x" : String(t + 1), 1);
        }
    }
    }

    /* === Row 5 (y=31): Mixer info === */
    if (!cueEditActive) {
    if (knobPage === KNOB_PAGE_MAIN) {
        /* Show crossfader position and volumes */
        let aVStr = "A:" + deckVol[0];
        let bVStr = "B:" + deckVol[1];
        let xfStr = "XF:" + crossfader;
        rPrint(1, 31, aVStr, 1);
        rPrint(30, 31, xfStr, 1);
        rPrint(65, 31, bVStr, 1);

        /* Vinyl speeds */
        rPrint(1, 39, "V:" + dkA.vinylSpeed, 1);
        rPrint(65, 39, "V:" + dkB.vinylSpeed, 1);
    } else if (knobPage === KNOB_PAGE_FX) {
        /* FX page: filters + crossfader */
        let fA = deck[0].filterPos;
        let fB = deck[1].filterPos;
        let fAStr = fA < 50 ? "LP" + (50-fA) : (fA > 50 ? "HP" + (fA-50) : "OFF");
        let fBStr = fB < 50 ? "LP" + (50-fB) : (fB > 50 ? "HP" + (fB-50) : "OFF");
        rPrint(1, 31, "F:" + fAStr, 1);
        rPrint(48, 31, "XF:" + crossfader, 1);
        rPrint(88, 31, "F:" + fBStr, 1);
    } else {
        /* Stems page: A1-A4 B1-B4 */
        for (let s = 0; s < 4; s++) {
            let av = deck[0].stemLoaded[s] ? String(deck[0].stemVols[s]) : "--";
            rPrint(s * 16, 31, "A" + (s+1) + av, 1);
        }
        for (let s = 0; s < 4; s++) {
            let bv = deck[1].stemLoaded[s] ? String(deck[1].stemVols[s]) : "--";
            rPrint(64 + s * 16, 31, "B" + (s+1) + bv, 1);
        }
    }
    } /* end !cueEditActive */

    /* === Row 6 (y=47): Page content for active deck (compact) === */
    drawPageContent();

    /* === Footer (y=57) === */
    rFillRect(0, 55, SCREEN_W, 1, 1);
    let dk = deck[activeDeck];
    let pageName = ["Cue", "Stut", "Loop", "Edit"][currentPage];
    let deckLabel = activeDeck === 0 ? "A" : "B";
    if (currentPage === PAGE_CUE_EDIT && dk.editCueIdx >= 0) {
        let pos = dk.cuePositions[dk.editCueIdx];
        let posStr = pos >= 0 ? (pos * 100).toFixed(1) + "%" : "---";
        let zStr = dk.wfZoomH > 1.05 ? " z" + dk.wfZoomH.toFixed(1) : "";
        rPrint(1, 57, deckLabel + " C" + (dk.editCueIdx + 1) + ":" + posStr + zStr, 1);
    } else {
        let knobLabel = ["Mix", "FX", "Stem"][knobPage];
        rPrint(1, 57, deckLabel + ":" + pageName, 1);
        rPrint(50, 57, "K:" + knobLabel, 1);
    }
    rPrint(108, 57, shiftHeld ? "SH" : "", 1);

    /* End-of-track warning flash */
    for (let wd = 0; wd < 2; wd++) {
        if (warnFlashState[wd]) {
            let wx = wd === 0 ? 0 : 64;
            /* Flash border around deck half */
            rDrawRect(wx, 0, 63, 56, 1);
            rDrawRect(wx + 1, 1, 61, 54, 1);
        }
    }

    rDrawOverlay();
}

function drawWaveform(x, y, w, h, dk) {
    /* Draw waveform overview with cue markers, supports zoom/scroll */
    rDrawRect(x, y, w, h, 1);

    let zoomH = dk.wfZoomH || 1.0;
    let zoomV = dk.wfZoomV || 1.0;
    let offset = dk.wfOffset || 0.0;
    let innerW = w - 2;
    let innerH = h - 2;

    if (dk.waveform) {
        for (let col = 0; col < innerW; col++) {
            /* Map pixel column to waveform position (0-1) accounting for zoom/scroll */
            let frac = offset + (col / innerW) / zoomH;
            let wfIdx = Math.floor(frac * 128);
            if (wfIdx < 0 || wfIdx >= 128) continue;
            let peak = dk.waveform[wfIdx] || 0;
            let barH = Math.min(innerH, Math.floor(peak * innerH * zoomV / 15));
            if (barH > 0) {
                rFillRect(x + 1 + col, y + h - 1 - barH, 1, barH, 1);
            }
        }
    }

    /* Map a track position (0-1) to pixel x, returns -1 if out of view */
    function posToX(pos) {
        let rel = (pos - offset) * zoomH;
        if (rel < 0 || rel > 1) return -1;
        return x + 1 + Math.floor(rel * innerW);
    }

    /* Draw cue markers as vertical lines */
    for (let c = 0; c < 8; c++) {
        if (dk.cuePositions[c] >= 0) {
            let cx = posToX(dk.cuePositions[c]);
            if (cx >= x + 1 && cx < x + w - 1) {
                if (dk.editCueIdx === c) {
                    /* Selected cue: solid line */
                    for (let cy = y + 1; cy < y + h - 1; cy++) {
                        rFillRect(cx, cy, 1, 1, 1);
                    }
                } else {
                    /* Other cue: dashed line */
                    for (let cy = y + 1; cy < y + h - 1; cy += 2) {
                        rFillRect(cx, cy, 1, 1, 1);
                    }
                }
            }
        }
    }

    /* Draw playback position */
    if (dk.totalFrames > 0) {
        let px = posToX(dk.playPos);
        if (px >= x + 1 && px < x + w - 1) {
            for (let py = y + 1; py < y + h - 1; py += 3) {
                rFillRect(px, py, 1, 2, 1);
            }
        }
    }
}

function drawPageContent() {
    let y = 47;
    let dk = deck[activeDeck];
    let deckLabel = activeDeck === 0 ? "A" : "B";

    if (currentPage === PAGE_HOT_CUE) {
        let s = deckLabel + ":";
        for (let c = 0; c < 8; c++) {
            s += (dk.cuePositions[c] >= 0) ? String(c + 1) : "-";
        }
        rPrint(1, y, s, 1);
    } else if (currentPage === PAGE_STUTTER) {
        if (dk.stutterActive) {
            let sizeLabel = STUTTER_LABELS[dk.stutterPadHeld] || "?";
            rPrint(1, y, deckLabel + " Stutter:" + sizeLabel + "b", 1);
        } else {
            rPrint(1, y, deckLabel + " Stutter:hold pad", 1);
        }
    } else if (currentPage === PAGE_LOOP) {
        let loopLabel = LOOP_LABELS[dk.loopSizeIdx] || "1";
        let status = dk.loopActive ? "ON " : "OFF";
        rPrint(1, y, deckLabel + " Loop:" + loopLabel + "b [" + status + "]", 1);
    } else if (currentPage === PAGE_CUE_EDIT) {
        /* Waveform fills rows 4-6 area (y=23 to y=53, 30px tall) */
        drawWaveform(0, 23, 128, 30, dk);
        if (dk.editCueIdx >= 0) {
            let pos = dk.cuePositions[dk.editCueIdx];
            let posStr = pos >= 0 ? (pos * 100).toFixed(1) + "%" : "---";
            /* Cue info shown in footer instead */
        } else {
            rPrint(1, 40, "Tap pad to edit", 1);
        }
    }
}

function drawBrowser() {
    clear_screen();

    let deckLabel = browserTargetDeck === 0 ? "A" : "B";
    let dirName = pathBasename(browserCurrentDir) || "Samples";
    drawMenuHeader("Deck " + deckLabel + ": " + truncStr(dirName, 11));

    if (!deck[browserTargetDeck].isMod) {
        rPrint(2, 14, "Stem " + (browserTargetStem + 1) + " (Trk=Change)", 1);
    } else {
        rPrint(2, 14, "Select file", 1);
    }

    let listY = 24;
    let lineH = 9;
    let maxVisible = Math.floor((55 - listY) / lineH);
    let startIdx = 0;
    if (browserSelectedIdx > maxVisible - 2) startIdx = browserSelectedIdx - (maxVisible - 2);
    let endIdx = Math.min(startIdx + maxVisible, browserItems.length);

    for (let i = startIdx; i < endIdx; i++) {
        let y = listY + (i - startIdx) * lineH;
        let isSel = (i === browserSelectedIdx);
        let label = isSel ? scrollStr(browserItems[i].label, 20) : truncStr(browserItems[i].label, 20);

        if (isSel) {
            rFillRect(0, y - 1, SCREEN_W, lineH, 1);
            rPrint(4, y, label, 0);
        } else {
            rPrint(4, y, label, 1);
        }
    }

    if (browserItems.length === 0) {
        rPrint(4, listY, "(empty)", 1);
    }

    rFillRect(0, 55, SCREEN_W, 1, 1);
    rPrint(2, 57, "Back", 1);
    rPrint(SCREEN_W - 7 * CHAR_W - 2, 57, "Jog:Sel", 1);
}

function drawDeckVertical() {
    clear_screen();
    let cw = canvasW(); /* 64 */
    let ch = canvasH(); /* 128 */
    let dkA = deck[0];
    let dkB = deck[1];

    /* === Deck A section (top half, y=0..58) === */
    let aPlay = dkA.playing ? ">" : "||";
    rPrint(1, 0, aPlay, 1);
    rPrint(14, 0, dkA.bpm.toFixed(1) + "BPM", 1);
    if (activeDeck === 0) rFillRect(0, 6, cw, 1, 1);

    /* A pitch/speed */
    let ap = dkA.pitchSemitones >= 0 ? "+" + dkA.pitchSemitones : String(dkA.pitchSemitones);
    rPrint(1, 8, ap + "ST " + dkA.speedPct + "%", 1);

    /* A position bar */
    rDrawRect(0, 15, cw, 4, 1);
    if (dkA.totalFrames > 0) {
        let posA = Math.floor(dkA.playPos * (cw - 2));
        rFillRect(1, 16, clamp(posA, 1, cw - 2), 2, 1);
    }

    /* A stems */
    for (let t = 0; t < 4; t++) {
        if (dkA.stemLoaded[t]) {
            rPrint(t * 16, 20, dkA.stemMutes[t] ? "X" : String(t + 1), 1);
        }
    }

    /* A filter/vinyl */
    let fA = dkA.filterPos;
    let fAStr = fA < 50 ? "LP" + (50 - fA) : (fA > 50 ? "HP" + (fA - 50) : "--");
    rPrint(1, 26, "F:" + fAStr, 1);
    rPrint(36, 26, "V:" + dkA.vinylSpeed, 1);

    /* === Mixer section (y=33..46) === */
    rFillRect(0, 33, cw, 1, 1);
    let aVStr = "A:" + deckVol[0];
    let bVStr = "B:" + deckVol[1];
    rPrint(1, 35, aVStr, 1);
    rPrint(32, 35, bVStr, 1);
    rPrint(1, 42, "XF:" + crossfader, 1);
    rPrint(32, 42, "M:" + masterVol, 1);
    rFillRect(0, 48, cw, 1, 1);

    /* === Deck B section (bottom half, y=49..110) === */
    let bPlay = dkB.playing ? ">" : "||";
    rPrint(1, 50, bPlay, 1);
    rPrint(14, 50, dkB.bpm.toFixed(1) + "BPM", 1);
    if (activeDeck === 1) rFillRect(0, 56, cw, 1, 1);

    /* B pitch/speed */
    let bp = dkB.pitchSemitones >= 0 ? "+" + dkB.pitchSemitones : String(dkB.pitchSemitones);
    rPrint(1, 58, bp + "ST " + dkB.speedPct + "%", 1);

    /* B position bar */
    rDrawRect(0, 65, cw, 4, 1);
    if (dkB.totalFrames > 0) {
        let posB = Math.floor(dkB.playPos * (cw - 2));
        rFillRect(1, 66, clamp(posB, 1, cw - 2), 2, 1);
    }

    /* B stems */
    for (let t = 0; t < 4; t++) {
        if (dkB.stemLoaded[t]) {
            rPrint(t * 16, 70, dkB.stemMutes[t] ? "X" : String(t + 1), 1);
        }
    }

    /* B filter/vinyl */
    let fB = dkB.filterPos;
    let fBStr = fB < 50 ? "LP" + (50 - fB) : (fB > 50 ? "HP" + (fB - 50) : "--");
    rPrint(1, 76, "F:" + fBStr, 1);
    rPrint(36, 76, "V:" + dkB.vinylSpeed, 1);

    /* === Page content (y=84) === */
    rFillRect(0, 83, cw, 1, 1);
    let dk = deck[activeDeck];
    let dl = activeDeck === 0 ? "A" : "B";
    if (currentPage === PAGE_HOT_CUE) {
        let cs = dl + ":";
        for (let c = 0; c < 8; c++) cs += (dk.cuePositions[c] >= 0) ? String(c + 1) : "-";
        rPrint(1, 85, cs, 1);
    } else if (currentPage === PAGE_STUTTER) {
        if (dk.stutterActive) {
            rPrint(1, 85, dl + " STT:" + (STUTTER_LABELS[dk.stutterPadHeld] || "?"), 1);
        } else {
            rPrint(1, 85, dl + " STUTTER", 1);
        }
    } else if (currentPage === PAGE_LOOP) {
        let ll = LOOP_LABELS[dk.loopSizeIdx] || "1";
        rPrint(1, 85, dl + " LP:" + ll + (dk.loopActive ? " ON" : " OFF"), 1);
    } else if (currentPage === PAGE_CUE_EDIT) {
        drawWaveform(0, 84, cw, 24, dk);
        if (dk.editCueIdx >= 0) {
            let pos = dk.cuePositions[dk.editCueIdx];
            let posStr = pos >= 0 ? (pos * 100).toFixed(1) + "%" : "---";
            rPrint(1, 110, "C" + (dk.editCueIdx + 1) + ":" + posStr, 1);
        } else {
            rPrint(1, 110, "SEL CUE", 1);
        }
    }

    /* === Footer (y=120) === */
    rFillRect(0, 119, cw, 1, 1);
    let pageName = ["CUE", "STT", "LOP", "EDT"][currentPage];
    let knobLabel = ["MIX", "FX", "STM"][knobPage];
    rPrint(1, 121, dl + ":" + pageName + " " + knobLabel, 1);

    /* End-of-track warning flash */
    for (let wd = 0; wd < 2; wd++) {
        if (warnFlashState[wd]) {
            let wy = wd === 0 ? 0 : 49;
            let wh = wd === 0 ? 32 : 33;
            rDrawRect(0, wy, cw, wh, 1);
            rDrawRect(1, wy + 1, cw - 2, wh - 2, 1);
        }
    }

    rDrawOverlay();
}

function drawBrowserVertical() {
    clear_screen();
    let cw = canvasW();

    let deckLabel = browserTargetDeck === 0 ? "A" : "B";
    let dirName = pathBasename(browserCurrentDir) || "SAMPLES";
    rPrint(1, 0, "DECK " + deckLabel, 1);
    rPrint(1, 7, truncStr(dirName, 12).toUpperCase(), 1);

    if (!deck[browserTargetDeck].isMod) {
        rPrint(1, 14, "STEM " + (browserTargetStem + 1), 1);
    }

    let listY = 22;
    let lineH = 7;
    let maxVisible = Math.floor((120 - listY) / lineH);
    let startIdx = 0;
    if (browserSelectedIdx > maxVisible - 2) startIdx = browserSelectedIdx - (maxVisible - 2);
    let endIdx = Math.min(startIdx + maxVisible, browserItems.length);

    for (let i = startIdx; i < endIdx; i++) {
        let y = listY + (i - startIdx) * lineH;
        let isSel = (i === browserSelectedIdx);
        let label;
        if (isSel) {
            label = scrollStr(browserItems[i].label, 12).toUpperCase();
        } else {
            label = truncStr(browserItems[i].label, 12).toUpperCase();
        }

        if (isSel) {
            rFillRect(0, y - 1, cw, lineH, 1);
            drawPixelText(2, y, label, 0);
        } else {
            rPrint(2, y, label, 1);
        }
    }

    if (browserItems.length === 0) {
        rPrint(2, listY, "(EMPTY)", 1);
    }

    rFillRect(0, 121, cw, 1, 1);
    rPrint(1, 123, "BACK    SEL", 1);
}

function draw() {
    if (currentView === VIEW_PLAYLIST) {
        drawPlaylist();
    } else if (currentView === VIEW_BROWSER) {
        if (displayRotation === 1 || displayRotation === 3) {
            drawBrowserVertical();
        } else {
            drawBrowser();
        }
    } else {
        if (displayRotation === 1 || displayRotation === 3) {
            drawDeckVertical();
        } else {
            drawDeck();
        }
    }
}

/* ============================================================================
 * MIDI Handling
 * ============================================================================ */

function handleCC(cc, val) {
    /* Shift */
    if (cc === MoveShift) {
        shiftHeld = val > 63;
        needsRedraw = true;
        return;
    }

    /* Back button */
    if (cc === MoveBack && val > 63) {
        if (currentView === VIEW_PLAYLIST) {
            playlistBack();
        } else if (currentView === VIEW_BROWSER) {
            browserBack();
        } else {
            let now = Date.now();
            if (backConfirmPending && (now - backConfirmTime) < BACK_CONFIRM_MS) {
                backConfirmPending = false;
                if (typeof host_exit_module === "function") host_exit_module();
            } else {
                backConfirmPending = true;
                backConfirmTime = now;
                announce("Press Back again to exit");
                showOverlay("Press Back again", "to exit DJ module", BACK_CONFIRM_MS);
            }
        }
        needsRedraw = true;
        return;
    }

    /* Copy button (above Shift): cycle display rotation */
    if (cc === MoveCopy && val > 63) {
        displayRotation = (displayRotation + 1) % 4;
        let labels = ["Normal", "90 CW", "180", "270 CW"];
        announce("Display " + labels[displayRotation]);
        showOverlay("Display", labels[displayRotation]);
        needsRedraw = true;
        return;
    }

    /* Play button: toggle Deck A play */
    if (cc === MovePlay && val > 63) {
        /* Sync from DSP first to avoid stale toggle after auto-stop */
        let v = host_module_get_param("a_playing");
        if (v) deck[0].playing = (v === "1");
        deck[0].playing = !deck[0].playing;
        sendParamNow("a_playing", deck[0].playing ? "1" : "0");
        announce("Deck A " + (deck[0].playing ? "playing" : "stopped"));
        updateControlLEDs();
        needsRedraw = true;
        return;
    }

    /* Rec button: toggle Deck B play */
    if (cc === MoveRec && val > 63) {
        let v = host_module_get_param("b_playing");
        if (v) deck[1].playing = (v === "1");
        deck[1].playing = !deck[1].playing;
        sendParamNow("b_playing", deck[1].playing ? "1" : "0");
        announce("Deck B " + (deck[1].playing ? "playing" : "stopped"));
        updateControlLEDs();
        needsRedraw = true;
        return;
    }

    /* Data knob turn */
    if (cc === MoveMainKnob) {
        let delta = decodeDelta(val);
        if (currentView === VIEW_PLAYLIST) {
            playlistNavigate(delta);
            needsRedraw = true;
        } else if (currentView === VIEW_BROWSER) {
            browserNavigate(delta);
            needsRedraw = true;
        } else if (shiftHeld) {
            /* Shift + data wheel: switch knob page */
            let newPage = clamp(knobPage + (delta > 0 ? 1 : -1), KNOB_PAGE_MAIN, KNOB_PAGE_STEMS);
            if (newPage !== knobPage) {
                knobPage = newPage;
                let kpNames = ["Main", "FX", "Stems"];
                announce(kpNames[knobPage] + " knob page");
                updatePageLEDs();
                needsRedraw = true;
            }
        } else if (currentPage === PAGE_CUE_EDIT && deck[activeDeck].editCueIdx >= 0) {
            /* Scrub cue position */
            let dk = deck[activeDeck];
            let ci = dk.editCueIdx;
            let step = 0.002 * (shiftHeld ? 0.1 : 1); /* fine scrub with shift */
            dk.cuePositions[ci] = clamp(dk.cuePositions[ci] + delta * step, 0, 1);
            sendParamNow(dp(activeDeck, "cue_set_pos"), ci + " " + dk.cuePositions[ci].toFixed(6));
            needsRedraw = true;
        } else {
            pendingJogDelta += delta;
        }
        return;
    }

    /* Data knob push */
    if (cc === MoveMainButton && val > 63) {
        if (backConfirmPending) {
            backConfirmPending = false;
            needsRedraw = true;
            return;
        }
        if (currentView === VIEW_PLAYLIST) {
            playlistSelect(shiftHeld ? 1 : activeDeck);
        } else if (currentView === VIEW_BROWSER) {
            browserSelect();
        } else if (shiftHeld) {
            /* Shift + push: load to deck B */
            openBrowser(1);
        } else {
            /* Normal push: load to deck A */
            openBrowser(0);
        }
        needsRedraw = true;
        return;
    }

    /* Track mute buttons */
    for (let t = 0; t < 4; t++) {
        if (cc === TRACK_CCS[t] && val > 63) {
            if (currentView === VIEW_BROWSER) {
                browserTargetStem = t;
            } else if (shiftHeld) {
                /* Shift + track: switch active deck */
                activeDeck = (activeDeck === 0) ? 1 : 0;
                announce("Deck " + (activeDeck === 0 ? "A" : "B"));
                updateAllLEDs();
            } else {
                /* Toggle mute on active deck */
                let dk = deck[activeDeck];
                dk.stemMutes[t] = dk.stemMutes[t] ? 0 : 1;
                sendParamNow(dp(activeDeck, "stem_mute_" + t), String(dk.stemMutes[t]));
                announce("Stem " + (t + 1) + (dk.stemMutes[t] ? " muted" : " unmuted"));
                updateTrackLEDs();
            }
            needsRedraw = true;
            return;
        }
    }

    /* Cue edit page gets its own knob mapping (knobs 71-78) */
    if (cc >= 71 && cc <= 79 && currentPage === PAGE_CUE_EDIT) {
        handleKnobCueEdit(cc, val);
        return;
    }

    /* Stutter page: K1=pitch, K2=filter for active deck */
    if (cc >= 71 && cc <= 79 && currentPage === PAGE_STUTTER) {
        handleKnobStutter(cc, val);
        return;
    }

    /* Knob handling depends on knob page */
    if (knobPage === KNOB_PAGE_MAIN) {
        handleKnobMain(cc, val);
    } else if (knobPage === KNOB_PAGE_FX) {
        handleKnobFx(cc, val);
    } else {
        handleKnobStems(cc, val);
    }
}

function handleKnobMain(cc, val) {
    let delta = decodeDelta(val);

    /* Knob 1: Pitch shift (active deck) */
    if (cc === MoveKnob1) {
        let dk = deck[activeDeck];
        dk.pitchSemitones = clamp(dk.pitchSemitones + delta, -24, 24);
        sendParamNow(dp(activeDeck, "pitch_semitones"), String(dk.pitchSemitones));
        needsRedraw = true;
        return;
    }

    /* Knob 2: Speed / timestretch (active deck) */
    if (cc === MoveKnob2) {
        let dk = deck[activeDeck];
        dk.speedPct = clamp(dk.speedPct + delta, 50, 200);
        sendParamNow(dp(activeDeck, "speed_pct"), String(dk.speedPct));
        needsRedraw = true;
        return;
    }

    /* Knob 3: Deck A volume */
    if (cc === MoveKnob3) {
        deckVol[0] = clamp(deckVol[0] + delta * 2, 0, 175);
        sendParamNow("a_vol", String(deckVol[0]));
        needsRedraw = true;
        return;
    }

    /* Knob 4: Crossfader */
    if (cc === MoveKnob4) {
        crossfader = clamp(crossfader + delta, 0, 100);
        sendParamNow("crossfader", String(crossfader));
        needsRedraw = true;
        return;
    }

    /* Knob 5: Deck B volume */
    if (cc === MoveKnob5) {
        deckVol[1] = clamp(deckVol[1] + delta * 2, 0, 175);
        sendParamNow("b_vol", String(deckVol[1]));
        needsRedraw = true;
        return;
    }

    /* Knob 6: Switch active deck (left=A, right=B) */
    if (cc === MoveKnob6) {
        let newDeck = delta < 0 ? 0 : 1;
        if (activeDeck !== newDeck) {
            activeDeck = newDeck;
            announce("Deck " + (activeDeck === 0 ? "A" : "B"));
            updateAllLEDs();
            needsRedraw = true;
        }
        return;
    }

    /* Knob 7: Deck A vinyl speed (pitch+rate) */
    if (cc === MoveKnob7) {
        deck[0].vinylSpeed = clamp(deck[0].vinylSpeed + delta, 50, 150);
        sendParamNow("a_vinyl_speed", String(deck[0].vinylSpeed));
        needsRedraw = true;
        return;
    }

    /* Knob 8: Deck B vinyl speed (pitch+rate) */
    if (cc === MoveKnob8) {
        deck[1].vinylSpeed = clamp(deck[1].vinylSpeed + delta, 50, 150);
        sendParamNow("b_vinyl_speed", String(deck[1].vinylSpeed));
        needsRedraw = true;
        return;
    }

    /* Hardware master knob: Master volume */
    if (cc === MoveMaster) {
        masterVol = clamp(masterVol + delta * 2, 0, 100);
        sendParamNow("master_vol", String(masterVol));
        needsRedraw = true;
        return;
    }
}

function handleKnobFx(cc, val) {
    let delta = decodeDelta(val);

    /* Knob 3: Deck A DJ filter (0=LPF, 50=bypass, 100=HPF) */
    if (cc === MoveKnob3) {
        deck[0].filterPos = clamp(deck[0].filterPos + delta, 0, 100);
        sendParamNow("a_filter", String(deck[0].filterPos));
        needsRedraw = true;
        return;
    }

    /* Knob 4: Crossfader (duplicate) */
    if (cc === MoveKnob4) {
        crossfader = clamp(crossfader + delta, 0, 100);
        sendParamNow("crossfader", String(crossfader));
        needsRedraw = true;
        return;
    }

    /* Knob 5: Deck B DJ filter */
    if (cc === MoveKnob5) {
        deck[1].filterPos = clamp(deck[1].filterPos + delta, 0, 100);
        sendParamNow("b_filter", String(deck[1].filterPos));
        needsRedraw = true;
        return;
    }

    /* Knobs 1, 2, 6, 7, 8 still work as main page */
    handleKnobMain(cc, val);
}

function handleKnobStems(cc, val) {
    let delta = decodeDelta(val);

    /* K1-K4: Deck A stem volumes, K5-K8: Deck B stem volumes */
    let allKnobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
                    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    for (let k = 0; k < 8; k++) {
        if (cc === allKnobs[k]) {
            let d = k < 4 ? 0 : 1;
            let s = k % 4;
            deck[d].stemVols[s] = clamp(deck[d].stemVols[s] + delta * 2, 0, 100);
            sendParamNow(dp(d, "stem_vol_" + s), String(deck[d].stemVols[s]));
            needsRedraw = true;
            return;
        }
    }
}

function handleKnobStutter(cc, val) {
    let delta = decodeDelta(val);
    let dk = deck[activeDeck];

    /* Knob 1: Stutter pitch shift (-12 to +12 semitones) */
    if (cc === MoveKnob1) {
        dk.stutterPitch = clamp(dk.stutterPitch + delta, -12, 12);
        sendParamNow(dp(activeDeck, "stutter_pitch"), String(dk.stutterPitch));
        needsRedraw = true;
        return;
    }

    /* Knob 2: Stutter filter (0=heavy LPF, 50=bypass, 100=heavy HPF) */
    if (cc === MoveKnob2) {
        dk.stutterFilter = clamp(dk.stutterFilter + delta, 0, 100);
        sendParamNow(dp(activeDeck, "stutter_filter"), String(dk.stutterFilter));
        needsRedraw = true;
        return;
    }

    /* Remaining knobs: fall through to main knob handler */
    handleKnobMain(cc, val);
}

function handleKnobCueEdit(cc, val) {
    let delta = decodeDelta(val);
    let dk = deck[activeDeck];

    /* Knob 1: Cue position scrub (coarse) */
    if (cc === MoveKnob1 && dk.editCueIdx >= 0) {
        let ci = dk.editCueIdx;
        dk.cuePositions[ci] = clamp(dk.cuePositions[ci] + delta * 0.002, 0, 1);
        sendParamNow(dp(activeDeck, "cue_set_pos"), ci + " " + dk.cuePositions[ci].toFixed(6));
        /* Auto-scroll waveform to keep cue visible */
        autoScrollToCue(dk);
        needsRedraw = true;
        return;
    }

    /* Knob 2: Cue position scrub (fine) */
    if (cc === MoveKnob2 && dk.editCueIdx >= 0) {
        let ci = dk.editCueIdx;
        dk.cuePositions[ci] = clamp(dk.cuePositions[ci] + delta * 0.0002, 0, 1);
        sendParamNow(dp(activeDeck, "cue_set_pos"), ci + " " + dk.cuePositions[ci].toFixed(6));
        autoScrollToCue(dk);
        needsRedraw = true;
        return;
    }

    /* Knob 3: Horizontal zoom (1x to 16x) */
    if (cc === MoveKnob3) {
        let oldZoom = dk.wfZoomH;
        dk.wfZoomH = clamp(dk.wfZoomH * (delta > 0 ? 1.15 : 0.87), 1.0, 16.0);
        /* Keep view centered when zooming */
        let center = dk.wfOffset + 0.5 / oldZoom;
        dk.wfOffset = clamp(center - 0.5 / dk.wfZoomH, 0, 1.0 - 1.0 / dk.wfZoomH);
        needsRedraw = true;
        return;
    }

    /* Knob 4: Vertical zoom (1x to 4x) */
    if (cc === MoveKnob4) {
        dk.wfZoomV = clamp(dk.wfZoomV + delta * 0.1, 1.0, 4.0);
        needsRedraw = true;
        return;
    }

    /* Knob 5: Waveform scroll */
    if (cc === MoveKnob5) {
        let maxOff = Math.max(0, 1.0 - 1.0 / dk.wfZoomH);
        dk.wfOffset = clamp(dk.wfOffset + delta * 0.01 / dk.wfZoomH, 0, maxOff);
        needsRedraw = true;
        return;
    }

    /* Knob 6: Deck select (always available) */
    if (cc === MoveKnob6) {
        let newDeck = delta < 0 ? 0 : 1;
        if (activeDeck !== newDeck) {
            activeDeck = newDeck;
            announce("Deck " + (activeDeck === 0 ? "A" : "B"));
            updateAllLEDs();
            needsRedraw = true;
        }
        return;
    }

    /* Hardware master: Master volume (always available) */
    if (cc === MoveMaster) {
        masterVol = clamp(masterVol + delta * 2, 0, 100);
        sendParamNow("master_vol", String(masterVol));
        needsRedraw = true;
        return;
    }
}

function autoScrollToCue(dk) {
    if (dk.editCueIdx < 0 || dk.cuePositions[dk.editCueIdx] < 0) return;
    let pos = dk.cuePositions[dk.editCueIdx];
    let viewStart = dk.wfOffset;
    let viewEnd = dk.wfOffset + 1.0 / dk.wfZoomH;
    if (pos < viewStart || pos > viewEnd) {
        dk.wfOffset = clamp(pos - 0.5 / dk.wfZoomH, 0, Math.max(0, 1.0 - 1.0 / dk.wfZoomH));
    }
}

function handleNoteOn(note, vel) {
    if (vel === 0) {
        handleNoteOff(note);
        return;
    }

    /* Step buttons: page selection (shared across both decks) */
    if (note >= STEP_BASE && note < STEP_BASE + 4) {
        /* Save cues for both decks when leaving cue edit page */
        if (currentPage === PAGE_CUE_EDIT) {
            queueParam(dp(0, "save_cues"), "1");
            queueParam(dp(1, "save_cues"), "1");
        }
        /* Force-release any held stutter/hot-loop on BOTH decks before switching.
         * Prevents stuck loops/stutters when user changes page while holding a pad. */
        for (let d = 0; d < 2; d++) {
            if (deck[d].stutterPadHeld >= 0) {
                deck[d].stutterActive = false;
                deck[d].stutterPadHeld = -1;
                sendParamNow(dp(d, "stutter_active"), "0");
            }
            if (deck[d].hotLoopHeld >= 0) {
                deck[d].hotLoopHeld = -1;
                deck[d].loopActive = false;
                sendParamNow(dp(d, "hot_loop"), "-1");
            }
        }
        currentPage = note - STEP_BASE;
        let padPageNames = ["Hot Cue", "Stutter", "Loop", "Cue Edit"];
        announce(padPageNames[currentPage] + " page");
        updatePageLEDs();
        updatePadLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 5-7: knob page shortcuts */
    if (note === STEP_BASE + 4) {
        knobPage = KNOB_PAGE_MAIN;
        announce("Main knob page");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }
    if (note === STEP_BASE + 5) {
        knobPage = KNOB_PAGE_FX;
        announce("FX knob page");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }
    if (note === STEP_BASE + 6) {
        knobPage = KNOB_PAGE_STEMS;
        announce("Stems knob page");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 8: toggle active deck */
    if (note === STEP_BASE + 7) {
        activeDeck = (activeDeck === 0) ? 1 : 0;
        announce("Deck " + (activeDeck === 0 ? "A" : "B"));
        updateAllLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 9 (note 24): Deck A slip toggle */
    if (note === STEP_BASE + 8) {
        deck[0].slipMode = !deck[0].slipMode;
        sendParamNow("a_slip_mode", deck[0].slipMode ? "1" : "0");
        announce("Deck A Slip " + (deck[0].slipMode ? "on" : "off"));
        showOverlay("Deck A Slip", deck[0].slipMode ? "ON" : "OFF");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 10 (note 25): Deck B slip toggle */
    if (note === STEP_BASE + 9) {
        deck[1].slipMode = !deck[1].slipMode;
        sendParamNow("b_slip_mode", deck[1].slipMode ? "1" : "0");
        announce("Deck B Slip " + (deck[1].slipMode ? "on" : "off"));
        showOverlay("Deck B Slip", deck[1].slipMode ? "ON" : "OFF");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 11 (note 26): empty gap between slip and sync */

    /* Step 12 (note 27): Deck A sync toggle */
    if (note === STEP_BASE + 11) {
        deck[0].syncMode = !deck[0].syncMode;
        if (deck[0].syncMode) performSync(0);
        announce("Deck A Sync " + (deck[0].syncMode ? "on" : "off"));
        showOverlay("Deck A Sync", deck[0].syncMode ? "ON" : "OFF");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 13 (note 28): Deck B sync toggle */
    if (note === STEP_BASE + 12) {
        deck[1].syncMode = !deck[1].syncMode;
        if (deck[1].syncMode) performSync(1);
        announce("Deck B Sync " + (deck[1].syncMode ? "on" : "off"));
        showOverlay("Deck B Sync", deck[1].syncMode ? "ON" : "OFF");
        updatePageLEDs();
        needsRedraw = true;
        return;
    }

    /* Step 16 (note 31): Playlist view toggle */
    if (note === STEP_BASE + 15) {
        if (currentView === VIEW_PLAYLIST) {
            currentView = VIEW_DECK;
            playlistPageActive = false;
            announce("Deck view");
        } else {
            openPlaylistView();
            announce("Playlists");
        }
        updatePageLEDs();
        needsRedraw = true;
        return;
    }

    /* Pad presses: route to correct deck */
    let info = padToDeck(note);
    if (info) {
        handlePadPress(info.deck, info.padIdx);
        return;
    }
}

function handleNoteOff(note) {
    let info = padToDeck(note);
    if (!info) return;
    let dk = deck[info.deck];

    /* Top pads: stutter release or hot loop release — no page check to prevent stuck state */
    if (info.padIdx >= NUM_PERF_PADS) {
        let topIdx = info.padIdx - NUM_PERF_PADS;
        /* Stutter release (stutter pads are now on top row) */
        if (dk.stutterPadHeld === topIdx) {
            dk.stutterActive = false;
            dk.stutterPadHeld = -1;
            sendParamNow(dp(info.deck, "stutter_active"), "0");
            updatePadLEDs();
            needsRedraw = true;
        }
        /* Hot loop release */
        if (dk.hotLoopHeld === topIdx) {
            dk.hotLoopHeld = -1;
            dk.loopActive = false;
            sendParamNow(dp(info.deck, "hot_loop"), "-1");
            updatePadLEDs();
            needsRedraw = true;
        }
    }
}

function handlePadPress(deckIdx, padIdx) {
    let dk = deck[deckIdx];

    /* Loop page uses all 16 pads */
    if (currentPage === PAGE_LOOP) {
        if (padIdx < NUM_LOOP) {
            if (dk.loopActive && dk.loopSizeIdx === padIdx) {
                /* Re-press active loop: toggle off */
                dk.loopActive = false;
                sendParamNow(dp(deckIdx, "hot_loop"), "-1");
                announce("Loop off");
            } else {
                /* Select new loop size and enable */
                dk.loopSizeIdx = padIdx;
                dk.loopActive = true;
                sendParamNow(dp(deckIdx, "hot_loop"), String(padIdx));
                announce("Loop " + LOOP_LABELS[padIdx]);
            }
            updatePadLEDs();
            needsRedraw = true;
        }
        return;
    }

    /* Top 8 pads */
    if (padIdx >= NUM_PERF_PADS) {
        if (currentPage === PAGE_HOT_CUE) {
            /* Hot loop (roll): hold to activate, release to deactivate */
            let hlIdx = padIdx - NUM_PERF_PADS; /* 0-7 */
            dk.hotLoopHeld = hlIdx;
            dk.loopSizeIdx = HOT_LOOP_IDX[hlIdx];
            dk.loopActive = true;
            announce("Roll " + HOT_LOOP_LABELS[hlIdx]);
            sendParamNow(dp(deckIdx, "hot_loop"), String(HOT_LOOP_IDX[hlIdx]));
            updatePadLEDs();
            needsRedraw = true;
        } else if (currentPage === PAGE_CUE_EDIT) {
            /* Top pads on cue edit: TRIGGER (jump to) hotcues for this deck */
            let cueIdx = padIdx - NUM_PERF_PADS; /* 0-7 */
            if (dk.cuePositions[cueIdx] >= 0) {
                if (dk.loopActive || dk.hotLoopHeld >= 0) {
                    dk.loopActive = false;
                    dk.hotLoopHeld = -1;
                    sendParamNow(dp(deckIdx, "hot_loop"), "-1");
                }
                sendParamNow(dp(deckIdx, "jump_cue"), String(cueIdx));
            }
            updatePadLEDs();
            needsRedraw = true;
        } else if (currentPage === PAGE_STUTTER) {
            /* Top pads on stutter page: stutter sizes (hold to stutter) */
            let stuttIdx = padIdx - NUM_PERF_PADS; /* 0-7 */
            if (stuttIdx < NUM_STUTTER) {
                dk.stutterPadHeld = stuttIdx;
                dk.stutterActive = true;
                announce("Stutter " + STUTTER_LABELS[stuttIdx]);
                /* Use stutter_go which atomically sets size AND activates.
                 * Two back-to-back sendParamNow calls get coalesced by the
                 * framework (only one param delivered per tick). */
                sendParamNow(dp(deckIdx, "stutter_go"), String(stuttIdx));
                updatePadLEDs();
                needsRedraw = true;
            }
        }
        return;
    }

    if (currentPage === PAGE_CUE_EDIT) {
        if (shiftHeld && dk.cuePositions[padIdx] >= 0) {
            /* Shift+pad: delete cue */
            sendParamNow(dp(deckIdx, "clear_cue"), String(padIdx));
            dk.cuePositions[padIdx] = -1;
            if (dk.editCueIdx === padIdx) dk.editCueIdx = -1;
            announce("Cue " + (padIdx + 1) + " cleared");
        } else {
            /* Save previous cue before switching */
            if (dk.editCueIdx >= 0 && dk.editCueIdx !== padIdx) {
                queueParam(dp(deckIdx, "save_cues"), "1");
            }
            /* Select cue for editing (create at current pos if empty) */
            if (dk.cuePositions[padIdx] < 0) {
                sendParamNow(dp(deckIdx, "set_cue"), String(padIdx));
                dk.cuePositions[padIdx] = dk.playPos;
                announce("Cue " + (padIdx + 1) + " created, editing");
            } else {
                announce("Editing cue " + (padIdx + 1));
            }
            dk.editCueIdx = padIdx;
            if (!dk.waveform) syncWaveform(deckIdx);
        }
        updatePadLEDs();
        needsRedraw = true;
        return;
    }

    if (currentPage === PAGE_HOT_CUE) {
        if (shiftHeld && dk.cuePositions[padIdx] >= 0) {
            sendParamNow(dp(deckIdx, "clear_cue"), String(padIdx));
            dk.cuePositions[padIdx] = -1;
            announce("Cue " + (padIdx + 1) + " cleared");
        } else if (dk.cuePositions[padIdx] >= 0) {
            /* Disable loop/hot-loop if active before jumping to cue */
            if (dk.loopActive || dk.hotLoopHeld >= 0) {
                dk.loopActive = false;
                dk.hotLoopHeld = -1;
                sendParamNow(dp(deckIdx, "hot_loop"), "-1");
            }
            sendParamNow(dp(deckIdx, "jump_cue"), String(padIdx));
            announce("Cue " + (padIdx + 1));
        } else {
            sendParamNow(dp(deckIdx, "set_cue"), String(padIdx));
            dk.cuePositions[padIdx] = dk.playPos;
            announce("Cue " + (padIdx + 1) + " set");
        }
        updatePadLEDs();
        needsRedraw = true;
    }
    else if (currentPage === PAGE_STUTTER) {
        /* Bottom pads on stutter page = hotcue triggers */
        if (dk.cuePositions[padIdx] >= 0) {
            if (dk.loopActive || dk.hotLoopHeld >= 0) {
                dk.loopActive = false;
                dk.hotLoopHeld = -1;
                sendParamNow(dp(deckIdx, "hot_loop"), "-1");
            }
            sendParamNow(dp(deckIdx, "jump_cue"), String(padIdx));
            announce("Cue " + (padIdx + 1));
        }
        updatePadLEDs();
        needsRedraw = true;
    }
    /* Loop page is handled at top of handlePadPress */
}

function handleAftertouch(note, pressure) {
    /* Move pads don't send aftertouch MIDI — this is a no-op.
     * Stutter pitch/filter are controlled by knobs instead. */
}

function onMidiMessage(msg) {
    if (!msg || msg.length < 3) return;

    let status = msg[0] & 0xF0;
    let d1 = msg[1];
    let d2 = msg[2];

    /* Handle knob touch before filtering (touches are filtered by default) */
    if (status === 0x90 && d2 > 0 && d1 >= 0 && d1 <= 7) {
        let now = Date.now();
        let last = knobLastTouch[d1] || 0;
        if (now - last < DOUBLE_TAP_MS) {
            /* Double-tap: reset value (context-dependent) */
            let dk = deck[activeDeck];
            if (d1 === MoveKnob1Touch && currentPage === PAGE_STUTTER) {
                dk.stutterPitch = 0;
                sendParamNow(dp(activeDeck, "stutter_pitch"), "0");
                announce("Stutter pitch reset");
            } else if (d1 === MoveKnob2Touch && currentPage === PAGE_STUTTER) {
                dk.stutterFilter = 50;
                sendParamNow(dp(activeDeck, "stutter_filter"), "50");
                announce("Stutter filter reset");
            } else if (d1 === MoveKnob1Touch) {
                dk.pitchSemitones = 0;
                sendParamNow(dp(activeDeck, "pitch_semitones"), "0");
                announce("Pitch reset");
            } else if (d1 === MoveKnob2Touch) {
                dk.speedPct = 100;
                sendParamNow(dp(activeDeck, "speed_pct"), "100");
                announce("Speed reset");
            } else if (d1 === MoveKnob7Touch) {
                deck[0].vinylSpeed = 100;
                sendParamNow("a_vinyl_speed", "100");
                announce("Deck A vinyl speed reset");
            } else if (d1 === MoveKnob8Touch) {
                deck[1].vinylSpeed = 100;
                sendParamNow("b_vinyl_speed", "100");
                announce("Deck B vinyl speed reset");
            }
            knobLastTouch[d1] = 0;
            needsRedraw = true;
        } else {
            /* Single tap: show knob function overlay */
            knobLastTouch[d1] = now;
            let label = getKnobLabel(d1);
            if (label) {
                announce(label);
                showOverlay("Knob " + (d1 + 1), label);
            }
            needsRedraw = true;
        }
        return;
    }

    if (shouldFilterMessage(msg)) return;

    /* Dismiss overlay on non-knob input (but not during back confirm) */
    let isKnobCC = (status === 0xB0 && d1 >= 71 && d1 <= 79);
    if (!isKnobCC && !backConfirmPending) {
        dismissOverlayOnInput();
    }

    if (status === 0xB0) {
        handleCC(d1, d2);
    } else if (status === 0x90) {
        handleNoteOn(d1, d2);
    } else if (status === 0x80) {
        handleNoteOff(d1);
    } else if (status === 0xA0) {
        /* Poly aftertouch: d1=note, d2=pressure (0-127) */
        handleAftertouch(d1, d2);
    } else if (status === 0xD0) {
        /* Channel pressure: d1=pressure — apply to any active stutter */
        handleAftertouch(-1, d1);
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

globalThis.init = function() {
    ledInitPending = true;
    ledInitIndex = 0;
    currentView = VIEW_DECK;
    knobPage = KNOB_PAGE_MAIN;
    activeDeck = 0;
    deck = [makeDeckState(), makeDeckState()];
    pendingJogDelta = 0;

    syncDeckFromDsp(0);
    syncDeckFromDsp(1);
    syncMixerFromDsp();
    syncStemInfo(0);
    syncStemInfo(1);
    syncCuePositions(0);
    syncCuePositions(1);

    announce("DJ Deck loaded");
};

globalThis.tick = function() {
    tickCount++;

    /* Progressive LED init */
    if (ledInitPending) {
        setupLedBatch();
        draw();
        return;
    }

    /* Process overlay timeout */
    if (tickOverlay()) {
        needsRedraw = true;
    }

    /* Send queued params (one per tick for shadow mode safety) */
    if (paramQueue.length > 0) {
        let p = paramQueue.shift();
        host_module_set_param(p[0], p[1]);
    }

    /* Apply accumulated jog input */
    if (pendingJogDelta !== 0 && currentView === VIEW_DECK) {
        pendingJogDelta = 0;
    }

    /* Periodic DSP sync */
    if (tickCount % 4 === 0) {
        syncDeckFromDsp(0);
        syncDeckFromDsp(1);
        syncMixerFromDsp();

        /* Continuous BPM sync */
        for (let sd = 0; sd < 2; sd++) {
            if (deck[sd].syncMode) performSync(sd);
        }
    }
    if (tickCount % 44 === 0) {
        syncStemInfo(0);
        syncStemInfo(1);
        syncCuePositions(0);
        syncCuePositions(1);
        /* Sync waveform for active deck if on cue edit page */
        let adk = deck[activeDeck];
        if (currentPage === PAGE_CUE_EDIT && !adk.waveform) {
            syncWaveform(activeDeck);
        }
    }

    /* End-of-track warning flash — use playPos fraction + totalFrames to compute seconds remaining */
    let now2 = Date.now();
    if (now2 - warnFlashTime >= WARN_FLASH_MS) {
        warnFlashTime = now2;
        for (let wd = 0; wd < 2; wd++) {
            let wdk = deck[wd];
            let shouldFlash = false;
            if (wdk.playing && wdk.playPos > 0.01) {
                if (wdk.totalFrames > 0) {
                    let secsLeft = (1.0 - wdk.playPos) * (wdk.totalFrames / 44100.0);
                    shouldFlash = secsLeft <= WARN_SECONDS && secsLeft > 0;
                } else {
                    /* Fallback: flash when past 90% with no frame count */
                    shouldFlash = wdk.playPos > 0.9;
                }
            }
            if (shouldFlash) {
                warnFlashState[wd] = !warnFlashState[wd];
                needsRedraw = true;
            } else if (warnFlashState[wd]) {
                warnFlashState[wd] = false;
                needsRedraw = true;
            }
        }
    }

    /* Periodic LED update */
    if (tickCount % 6 === 0) {
        updatePadLEDs();
        updateTrackLEDs();
        updateControlLEDs();
        needsRedraw = true;
    }

    /* Force continuous redraw while text is scrolling */
    if (scrollTextActive) needsRedraw = true;

    if (needsRedraw) {
        let now = Date.now();
        if (now - lastDrawTime >= MIN_DRAW_INTERVAL) {
            draw();
            needsRedraw = false;
            lastDrawTime = now;
        }
    }
};

globalThis.onMidiMessageInternal = onMidiMessage;
globalThis.onMidiMessageExternal = function(msg) {
    onMidiMessage(msg);
};
