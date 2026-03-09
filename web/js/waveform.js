/*
 * Canvas waveform rendering.
 * Draws mirrored amplitude display with cue markers.
 */

const WAVEFORM_COLOR = '#4fc3f7';
const WAVEFORM_BG = '#1a1a2e';
const PLAYHEAD_COLOR = '#ffffff';

const CUE_COLORS = [
    '#ff4444', '#ff8800', '#ffdd00', '#44ff44',
    '#44ddff', '#4488ff', '#aa44ff', '#ff44aa'
];

/**
 * Render waveform peaks onto a canvas.
 * @param {HTMLCanvasElement} canvas
 * @param {Float32Array} peaks - normalized peak values
 * @param {object} opts - { playPos, cues, zoomStart, zoomEnd, selectedCue }
 */
export function drawWaveform(canvas, peaks, opts = {}) {
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const midY = h / 2;

    const zoomStart = opts.zoomStart || 0;
    const zoomEnd = opts.zoomEnd || 1;
    const zoomRange = zoomEnd - zoomStart;

    // Background
    ctx.fillStyle = WAVEFORM_BG;
    ctx.fillRect(0, 0, w, h);

    if (!peaks || peaks.length === 0) return;

    // Draw waveform
    const startBin = Math.floor(zoomStart * peaks.length);
    const endBin = Math.ceil(zoomEnd * peaks.length);
    const binsInView = endBin - startBin;

    ctx.fillStyle = WAVEFORM_COLOR;
    for (let x = 0; x < w; x++) {
        const binIdx = startBin + Math.floor((x / w) * binsInView);
        if (binIdx >= peaks.length) break;
        const amp = peaks[binIdx] * midY * 0.9;
        ctx.fillRect(x, midY - amp, 1, amp * 2);
    }

    // Draw beat grid lines
    if (opts.beatgridDownbeat >= 0 && opts.beatgridBpm > 0 && opts.totalFrames > 0 && opts.sampleRate > 0) {
        const beatFrames = (60.0 / opts.beatgridBpm) * opts.sampleRate;
        const downbeatNorm = opts.beatgridDownbeat; // already 0-1
        const beatNorm = beatFrames / opts.totalFrames;
        ctx.strokeStyle = 'rgba(255,255,255,0.12)';
        ctx.lineWidth = 1;
        // Draw beats before and after downbeat
        let pos = downbeatNorm;
        while (pos > zoomStart) pos -= beatNorm;
        while (pos < zoomEnd) {
            if (pos >= zoomStart) {
                const x = ((pos - zoomStart) / zoomRange) * w;
                // Downbeat (every 4 beats from the reference) gets brighter
                const beatsFromDown = Math.round((pos - downbeatNorm) / beatNorm);
                const isDownbeat = (beatsFromDown % 4 === 0);
                ctx.strokeStyle = isDownbeat ? 'rgba(255,255,255,0.25)' : 'rgba(255,255,255,0.08)';
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, h);
                ctx.stroke();
            }
            pos += beatNorm;
        }
    }

    // Draw cue markers
    if (opts.cues) {
        for (let i = 0; i < opts.cues.length; i++) {
            const cuePos = opts.cues[i];
            if (cuePos < 0) continue;
            const normPos = cuePos; // already 0-1 normalized
            if (normPos < zoomStart || normPos > zoomEnd) continue;
            const x = ((normPos - zoomStart) / zoomRange) * w;

            ctx.strokeStyle = CUE_COLORS[i % CUE_COLORS.length];
            ctx.lineWidth = opts.selectedCue === i ? 2 : 1;
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, h);
            ctx.stroke();

            // Label
            ctx.fillStyle = CUE_COLORS[i % CUE_COLORS.length];
            ctx.font = '10px monospace';
            ctx.fillText(String(i + 1), x + 2, 10);
        }
    }

    // Draw playhead
    if (opts.playPos !== undefined && opts.playPos >= 0) {
        const normPos = opts.playPos;
        if (normPos >= zoomStart && normPos <= zoomEnd) {
            const x = ((normPos - zoomStart) / zoomRange) * w;
            ctx.strokeStyle = PLAYHEAD_COLOR;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, h);
            ctx.stroke();
        }
    }

    // Center line
    ctx.strokeStyle = 'rgba(255,255,255,0.15)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, midY);
    ctx.lineTo(w, midY);
    ctx.stroke();
}

export { CUE_COLORS };
