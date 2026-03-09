/*
 * Web Audio playback + decode.
 * Handles audio file loading, decoding, and preview playback.
 */

let audioCtx = null;
let currentSource = null;
let currentBuffer = null;
let startTime = 0;
let startOffset = 0;
let isPlaying = false;

export function getAudioContext() {
    if (!audioCtx) audioCtx = new AudioContext();
    return audioCtx;
}

/** Decode an ArrayBuffer into an AudioBuffer. */
export async function decodeAudio(arrayBuffer) {
    const ctx = getAudioContext();
    return ctx.decodeAudioData(arrayBuffer);
}

/** Get audio metadata from an AudioBuffer. */
export function getAudioInfo(audioBuffer) {
    return {
        duration: audioBuffer.duration,
        sampleRate: audioBuffer.sampleRate,
        totalFrames: audioBuffer.length,
        channels: audioBuffer.numberOfChannels,
    };
}

/** Compute waveform peaks for rendering. numBins = number of output bins. */
export function computePeaks(audioBuffer, numBins) {
    const channel = audioBuffer.getChannelData(0);
    const samplesPerBin = Math.floor(channel.length / numBins);
    const peaks = new Float32Array(numBins);
    for (let i = 0; i < numBins; i++) {
        let max = 0;
        const start = i * samplesPerBin;
        const end = Math.min(start + samplesPerBin, channel.length);
        for (let j = start; j < end; j++) {
            const abs = Math.abs(channel[j]);
            if (abs > max) max = abs;
        }
        peaks[i] = max;
    }
    return peaks;
}

/** Start playback from a given time offset (seconds). */
export function play(audioBuffer, offset = 0) {
    stop();
    const ctx = getAudioContext();
    currentBuffer = audioBuffer;
    currentSource = ctx.createBufferSource();
    currentSource.buffer = audioBuffer;
    currentSource.connect(ctx.destination);
    currentSource.onended = () => { isPlaying = false; };
    startOffset = offset;
    startTime = ctx.currentTime;
    currentSource.start(0, offset);
    isPlaying = true;
}

/** Stop playback. */
export function stop() {
    if (currentSource) {
        try { currentSource.stop(); } catch (e) { /* already stopped */ }
        currentSource.disconnect();
        currentSource = null;
    }
    isPlaying = false;
}

/** Get current playback position in seconds. */
export function getCurrentTime() {
    if (!isPlaying || !audioCtx) return startOffset;
    return startOffset + (audioCtx.currentTime - startTime);
}

/** Is audio currently playing? */
export function playing() {
    return isPlaying;
}
