/*
 * Waveform editor — canvas rendering with cue markers, click-to-set, zoom.
 */

import { state, loadData } from '../app.js';
import * as db from '../db.js';
import { drawWaveform } from '../waveform.js';
import * as audio from '../audio.js';
import { refreshLibrary } from './library.js';

let currentTrack = null;
let peaks = null;
let zoomStart = 0;
let zoomEnd = 1;
let selectedCue = -1;
let animFrame = null;
let isDragging = false;

export function initWaveformEditor() {
    const canvas = document.getElementById('waveform-canvas');

    // Track loaded
    document.addEventListener('track-detail-loaded', (e) => {
        currentTrack = e.detail.track;
        peaks = currentTrack.waveformPeaks ? new Float32Array(currentTrack.waveformPeaks) : null;
        zoomStart = 0;
        zoomEnd = 1;
        selectedCue = -1;
        resizeCanvas();
        render();
    });

    // Cue selection from detail panel
    document.addEventListener('cue-selected', (e) => {
        selectedCue = e.detail.index;
        render();
    });

    // Click to set/move cue
    canvas.addEventListener('mousedown', onMouseDown);
    canvas.addEventListener('mousemove', onMouseMove);
    canvas.addEventListener('mouseup', onMouseUp);

    // Touch support
    canvas.addEventListener('touchstart', onTouchStart, { passive: false });
    canvas.addEventListener('touchmove', onTouchMove, { passive: false });
    canvas.addEventListener('touchend', onTouchEnd);

    // Zoom
    document.getElementById('btn-zoom-in').addEventListener('click', () => zoom(0.5));
    document.getElementById('btn-zoom-out').addEventListener('click', () => zoom(2));
    document.getElementById('btn-zoom-reset').addEventListener('click', () => {
        zoomStart = 0; zoomEnd = 1; render();
    });

    // Wheel zoom
    canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        const factor = e.deltaY > 0 ? 1.2 : 0.8;
        const mouseX = e.offsetX / canvas.clientWidth;
        zoomAround(mouseX, factor);
        render();
    });

    // Resize observer
    new ResizeObserver(() => { resizeCanvas(); render(); }).observe(canvas);

    // Animation loop for playback position
    startAnimLoop();
}

function resizeCanvas() {
    const canvas = document.getElementById('waveform-canvas');
    canvas.width = canvas.clientWidth * (window.devicePixelRatio || 1);
    canvas.height = canvas.clientHeight * (window.devicePixelRatio || 1);
}

function render() {
    const canvas = document.getElementById('waveform-canvas');
    if (!canvas.width) return;

    const ctx = canvas.getContext('2d');
    ctx.save();
    ctx.scale(window.devicePixelRatio || 1, window.devicePixelRatio || 1);

    // Normalize cues to 0-1
    let normalizedCues = null;
    if (currentTrack?.cues) {
        normalizedCues = currentTrack.cues.map(c => c); // already 0-1
    }

    const playPos = audio.playing() && currentTrack?.duration
        ? audio.getCurrentTime() / currentTrack.duration
        : -1;

    drawWaveform(canvas, peaks, {
        playPos,
        cues: normalizedCues,
        zoomStart,
        zoomEnd,
        selectedCue,
        beatgridDownbeat: currentTrack?.beatgridDownbeat ?? -1,
        beatgridBpm: currentTrack?.beatgridBpm ?? 0,
        totalFrames: currentTrack?.totalFrames ?? 0,
        sampleRate: currentTrack?.sampleRate ?? 44100,
    });

    ctx.restore();
}

function startAnimLoop() {
    function tick() {
        if (audio.playing()) render();
        animFrame = requestAnimationFrame(tick);
    }
    tick();
}

// ---- Interaction ----

function canvasToNorm(e) {
    const canvas = document.getElementById('waveform-canvas');
    const rect = canvas.getBoundingClientRect();
    const x = (e.clientX - rect.left) / rect.width;
    return zoomStart + x * (zoomEnd - zoomStart);
}

function onMouseDown(e) {
    if (selectedCue < 0 || !currentTrack) return;
    isDragging = true;
    setCueAtPosition(canvasToNorm(e));
}

function onMouseMove(e) {
    if (!isDragging) return;
    setCueAtPosition(canvasToNorm(e));
}

function onMouseUp() {
    if (isDragging) {
        isDragging = false;
        saveCues();
    }
}

function onTouchStart(e) {
    if (selectedCue < 0 || !currentTrack) return;
    e.preventDefault();
    isDragging = true;
    const touch = e.touches[0];
    setCueAtPosition(canvasToNormTouch(touch));
}

function onTouchMove(e) {
    if (!isDragging) return;
    e.preventDefault();
    const touch = e.touches[0];
    setCueAtPosition(canvasToNormTouch(touch));
}

function onTouchEnd() {
    if (isDragging) {
        isDragging = false;
        saveCues();
    }
}

function canvasToNormTouch(touch) {
    const canvas = document.getElementById('waveform-canvas');
    const rect = canvas.getBoundingClientRect();
    const x = (touch.clientX - rect.left) / rect.width;
    return zoomStart + x * (zoomEnd - zoomStart);
}

function setCueAtPosition(normPos) {
    if (selectedCue < 0 || !currentTrack) return;
    normPos = Math.max(0, Math.min(1, normPos));
    if (!currentTrack.cues) currentTrack.cues = [-1, -1, -1, -1, -1, -1, -1, -1];
    currentTrack.cues[selectedCue] = normPos;
    render();
    // Update detail panel
    document.dispatchEvent(new CustomEvent('track-detail-loaded', { detail: { track: currentTrack } }));
}

async function saveCues() {
    if (!currentTrack) return;
    currentTrack.dateModified = new Date().toISOString();
    await db.putTrack(currentTrack);
    await loadData();
}

// ---- Zoom ----

function zoom(factor) {
    const center = (zoomStart + zoomEnd) / 2;
    zoomAround(0.5, factor);
    render();
}

function zoomAround(mouseX, factor) {
    const range = zoomEnd - zoomStart;
    const anchor = zoomStart + mouseX * range;
    const newRange = Math.min(1, Math.max(0.01, range * factor));
    zoomStart = Math.max(0, anchor - mouseX * newRange);
    zoomEnd = Math.min(1, zoomStart + newRange);
    if (zoomEnd > 1) { zoomEnd = 1; zoomStart = Math.max(0, 1 - newRange); }
}
