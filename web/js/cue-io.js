/*
 * .cue file I/O — matches the Move's per-song cue format.
 *
 * Format: 8 lines, each a frame position (float, -1 = unset).
 * Filename: {djb2_hash}.cue
 */

import { djb2 } from './djb2.js';

export function cueFileName(filePath) {
    return djb2(filePath) + '.cue';
}

/** Parse a .cue file string into { cues: [8], beatgridDownbeat, beatgridBpm }.
 *  Lines 1-8: frame positions. Lines 9-10 (optional): beatgrid downbeat + BPM. */
export function parseCueFile(text) {
    const lines = text.trim().split('\n');
    const cues = [];
    for (let i = 0; i < 8; i++) {
        const val = i < lines.length ? parseFloat(lines[i]) : -1;
        cues.push(isNaN(val) ? -1 : val);
    }
    const beatgridDownbeat = lines.length > 8 ? parseFloat(lines[8]) : -1;
    const beatgridBpm = lines.length > 9 ? parseFloat(lines[9]) : 0;
    return {
        cues,
        beatgridDownbeat: isNaN(beatgridDownbeat) ? -1 : beatgridDownbeat,
        beatgridBpm: isNaN(beatgridBpm) ? 0 : beatgridBpm,
    };
}

/** Generate .cue file content from cues array + optional beatgrid.
 *  @param {number[]} cues - 8 frame positions
 *  @param {number} [beatgridDownbeat=-1] - frame position of first downbeat
 *  @param {number} [beatgridBpm=0] - exact BPM (0 = not set) */
export function generateCueFile(cues, beatgridDownbeat = -1, beatgridBpm = 0) {
    const out = [];
    for (let i = 0; i < 8; i++) {
        out.push(String(cues[i] !== undefined && cues[i] !== null ? cues[i] : -1));
    }
    out.push(String(Math.round(beatgridDownbeat)));
    out.push(beatgridBpm > 0 ? beatgridBpm.toFixed(2) : '0.00');
    return out.join('\n') + '\n';
}

/** Convert frame position to seconds (at 44100 Hz sample rate). */
export function framesToSeconds(frames, sampleRate = 44100) {
    return frames / sampleRate;
}

/** Convert seconds to frame position. */
export function secondsToFrames(seconds, sampleRate = 44100) {
    return Math.round(seconds * sampleRate);
}

/** Format seconds as MM:SS.mmm */
export function formatTime(seconds) {
    if (seconds < 0) return '--:--';
    const m = Math.floor(seconds / 60);
    const s = seconds % 60;
    return String(m).padStart(2, '0') + ':' + s.toFixed(3).padStart(6, '0');
}

/** Format seconds as MM:SS */
export function formatTimeShort(seconds) {
    if (seconds < 0) return '--:--';
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
}
