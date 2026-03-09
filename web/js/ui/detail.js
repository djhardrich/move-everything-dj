/*
 * Track detail — metadata editor, cue list, playback controls.
 */

import { state, loadData } from '../app.js';
import * as db from '../db.js';
import * as audio from '../audio.js';
import { formatTime, formatTimeShort, framesToSeconds } from '../cue-io.js';
import { refreshLibrary } from './library.js';
import { CUE_COLORS } from '../waveform.js';

let selectedCue = -1;
let playbackTimer = null;

export function initDetail() {
    // Listen for track selection
    document.addEventListener('track-selected', (e) => {
        showTrackDetail(e.detail.trackId);
    });

    // Metadata save on change
    ['meta-title', 'meta-artist', 'meta-bpm', 'meta-key', 'meta-comment',
     'meta-beatgrid-bpm', 'meta-beatgrid-downbeat'].forEach(id => {
        const el = document.getElementById(id);
        if (el) el.addEventListener('change', saveMetadata);
    });

    // Playback
    document.getElementById('btn-play').addEventListener('click', playTrack);
    document.getElementById('btn-stop').addEventListener('click', stopTrack);

    // Delete
    document.getElementById('btn-delete-track').addEventListener('click', deleteCurrentTrack);
}

async function showTrackDetail(trackId) {
    const detail = document.getElementById('detail');
    if (!trackId) { detail.classList.add('hidden'); return; }

    const track = await db.getTrack(trackId);
    if (!track) { detail.classList.add('hidden'); return; }

    detail.classList.remove('hidden');

    document.getElementById('meta-title').value = track.title || '';
    document.getElementById('meta-artist').value = track.artist || '';
    document.getElementById('meta-bpm').value = track.bpm || '';
    document.getElementById('meta-key').value = track.key || '';
    document.getElementById('meta-comment').value = track.comment || '';
    document.getElementById('meta-filepath').value = track.filePath || '';
    document.getElementById('meta-hash').value = track.cueFileHash || '';

    // Beat grid fields
    const bgBpmEl = document.getElementById('meta-beatgrid-bpm');
    const bgDownbeatEl = document.getElementById('meta-beatgrid-downbeat');
    if (bgBpmEl) bgBpmEl.value = track.beatgridBpm > 0 ? track.beatgridBpm.toFixed(2) : '';
    if (bgDownbeatEl) {
        if (track.beatgridDownbeat >= 0 && track.totalFrames > 0) {
            bgDownbeatEl.value = framesToSeconds(track.beatgridDownbeat * track.totalFrames).toFixed(3);
        } else {
            bgDownbeatEl.value = '';
        }
    }

    updateCueList(track);
    updatePlaybackTime(track);

    // Notify waveform editor
    document.dispatchEvent(new CustomEvent('track-detail-loaded', { detail: { track } }));
}

function updateCueList(track) {
    const container = document.getElementById('cue-list');
    container.innerHTML = '';

    for (let i = 0; i < 8; i++) {
        const chip = document.createElement('span');
        const hasValue = track.cues && track.cues[i] >= 0;
        chip.className = 'cue-chip' + (hasValue ? '' : ' empty') + (selectedCue === i ? ' selected' : '');
        chip.style.background = hasValue ? CUE_COLORS[i] + '33' : 'var(--bg-tertiary)';
        chip.style.color = hasValue ? CUE_COLORS[i] : 'var(--text-dim)';

        let label = `${i + 1}`;
        if (hasValue && track.totalFrames > 0) {
            const seconds = framesToSeconds(track.cues[i] * track.totalFrames);
            label += ` ${formatTimeShort(seconds)}`;
        }

        chip.innerHTML = label;
        if (hasValue) {
            const del = document.createElement('span');
            del.className = 'cue-delete';
            del.textContent = '\u00D7';
            del.addEventListener('click', async (e) => {
                e.stopPropagation();
                track.cues[i] = -1;
                track.dateModified = new Date().toISOString();
                await db.putTrack(track);
                await loadData();
                showTrackDetail(track.id);
            });
            chip.appendChild(del);
        }

        chip.addEventListener('click', () => {
            selectedCue = (selectedCue === i) ? -1 : i;
            updateCueList(track);
            document.dispatchEvent(new CustomEvent('cue-selected', { detail: { index: selectedCue } }));
        });

        container.appendChild(chip);
    }
}

function updatePlaybackTime(track) {
    const timeEl = document.getElementById('playback-time');
    const cur = audio.playing() ? audio.getCurrentTime() : 0;
    timeEl.textContent = formatTimeShort(cur) + ' / ' + formatTimeShort(track?.duration || 0);
}

async function playTrack() {
    const track = state.tracks.find(t => t.id === state.selectedTrackId);
    if (!track) return;

    const blob = await db.getAudioBlob(track.id);
    if (!blob) { alert('No audio data stored for this track'); return; }

    const audioBuffer = await audio.decodeAudio(blob);
    audio.play(audioBuffer);

    clearInterval(playbackTimer);
    playbackTimer = setInterval(() => {
        updatePlaybackTime(track);
        if (!audio.playing()) clearInterval(playbackTimer);
    }, 100);
}

function stopTrack() {
    audio.stop();
    clearInterval(playbackTimer);
    const track = state.tracks.find(t => t.id === state.selectedTrackId);
    updatePlaybackTime(track);
}

async function saveMetadata() {
    const track = await db.getTrack(state.selectedTrackId);
    if (!track) return;

    track.title = document.getElementById('meta-title').value;
    track.artist = document.getElementById('meta-artist').value;
    track.bpm = parseFloat(document.getElementById('meta-bpm').value) || 0;
    track.key = document.getElementById('meta-key').value;
    track.comment = document.getElementById('meta-comment').value;

    // Beat grid
    const bgBpmEl = document.getElementById('meta-beatgrid-bpm');
    const bgDownbeatEl = document.getElementById('meta-beatgrid-downbeat');
    if (bgBpmEl) track.beatgridBpm = parseFloat(bgBpmEl.value) || 0;
    if (bgDownbeatEl && track.totalFrames > 0) {
        const secs = parseFloat(bgDownbeatEl.value);
        track.beatgridDownbeat = (!isNaN(secs) && secs >= 0) ? (secs * (track.sampleRate || 44100)) / track.totalFrames : -1;
    }

    track.dateModified = new Date().toISOString();

    await db.putTrack(track);
    await loadData();
    refreshLibrary();
}

async function deleteCurrentTrack() {
    const track = state.tracks.find(t => t.id === state.selectedTrackId);
    if (!track) return;
    if (!confirm(`Delete "${track.title || track.fileName}"?`)) return;

    await db.deleteTrack(track.id);
    await db.deleteAudioBlob(track.id);
    state.selectedTrackId = null;
    document.getElementById('detail').classList.add('hidden');
    await loadData();
    refreshLibrary();
}

export function getSelectedCue() { return selectedCue; }
