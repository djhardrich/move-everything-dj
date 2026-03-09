/*
 * Toolbar — search, import/export buttons, settings, drag-and-drop.
 */

import { state, loadData, selectTrack } from '../app.js';
import * as db from '../db.js';
import { decodeAudio, getAudioInfo, computePeaks } from '../audio.js';
import { djb2 } from '../djb2.js';
import { parseCueFile, generateCueFile, cueFileName } from '../cue-io.js';
import { refreshLibrary } from './library.js';
import { refreshPlaylists } from './playlists.js';

export function initToolbar() {
    const searchInput = document.getElementById('search-input');
    searchInput.addEventListener('input', () => {
        state.searchQuery = searchInput.value;
        refreshLibrary();
    });

    document.getElementById('btn-import-files').addEventListener('click', () => {
        document.getElementById('file-input-audio').click();
    });
    document.getElementById('file-input-audio').addEventListener('change', handleAudioFiles);

    document.getElementById('btn-import-cue').addEventListener('click', () => {
        document.getElementById('file-input-cue').click();
    });
    document.getElementById('file-input-cue').addEventListener('change', handleCueFiles);

    document.getElementById('btn-import-djlib').addEventListener('click', () => {
        document.getElementById('file-input-djlib').click();
    });
    document.getElementById('file-input-djlib').addEventListener('change', handleDjlibImport);

    document.getElementById('btn-export-djlib').addEventListener('click', exportDjlib);
    document.getElementById('btn-export-cue').addEventListener('click', exportCueZip);
    document.getElementById('btn-export-playlists').addEventListener('click', exportPlaylistTxt);
    document.getElementById('btn-export-bundle').addEventListener('click', exportBundle);

    // Settings
    document.getElementById('btn-settings').addEventListener('click', openSettings);
    document.getElementById('btn-settings-cancel').addEventListener('click', closeSettings);
    document.getElementById('btn-settings-save').addEventListener('click', saveSettings);

    // Sidebar toggle (mobile)
    document.getElementById('btn-sidebar-toggle').addEventListener('click', () => {
        document.getElementById('sidebar').classList.toggle('mobile-open');
    });

    // Drag and drop
    initDragDrop();

    // Load saved settings
    const savedRoot = localStorage.getItem('moveLibraryRoot');
    if (savedRoot) state.moveLibraryRoot = savedRoot;
}

// ---- Audio file import ----

async function handleAudioFiles(e) {
    const files = Array.from(e.target.files);
    if (!files.length) return;
    for (const file of files) {
        await importAudioFile(file);
    }
    await loadData();
    refreshLibrary();
    e.target.value = '';
}

export async function importAudioFile(file) {
    const arrayBuffer = await file.arrayBuffer();

    // Decode audio
    let audioInfo;
    let peaks;
    try {
        const audioBuffer = await decodeAudio(arrayBuffer);
        audioInfo = getAudioInfo(audioBuffer);
        peaks = Array.from(computePeaks(audioBuffer, 800));
    } catch (err) {
        console.error('Failed to decode:', file.name, err);
        audioInfo = { duration: 0, sampleRate: 44100, totalFrames: 0, channels: 0 };
        peaks = [];
    }

    // Compute Move file path for hashing
    const movePath = state.moveLibraryRoot + '/' + file.name;
    const hash = djb2(movePath);

    const track = {
        id: db.uuid(),
        filePath: movePath,
        fileName: file.name,
        title: file.name.replace(/\.[^.]+$/, ''),
        artist: '',
        bpm: 0,
        key: '',
        duration: audioInfo.duration,
        sampleRate: audioInfo.sampleRate,
        totalFrames: audioInfo.totalFrames,
        comment: '',
        color: '',
        dateAdded: new Date().toISOString(),
        dateModified: new Date().toISOString(),
        cueFileHash: hash,
        cues: [-1, -1, -1, -1, -1, -1, -1, -1],
        beatgridDownbeat: -1,
        beatgridBpm: 0,
        waveformPeaks: peaks,
    };

    await db.putTrack(track);
    await db.putAudioBlob(track.id, arrayBuffer);
    return track;
}

// ---- .cue file import ----

async function handleCueFiles(e) {
    const files = Array.from(e.target.files);
    for (const file of files) {
        const text = await file.text();
        const parsed = parseCueFile(text);
        const hash = file.name.replace(/\.cue$/, '');

        // Find matching track by hash
        const track = state.tracks.find(t => t.cueFileHash === hash);
        if (track) {
            // Convert frame positions to normalized 0-1 range
            for (let i = 0; i < 8; i++) {
                if (parsed.cues[i] >= 0 && track.totalFrames > 0) {
                    track.cues[i] = parsed.cues[i] / track.totalFrames;
                } else {
                    track.cues[i] = parsed.cues[i];
                }
            }
            // Import beatgrid data
            if (parsed.beatgridDownbeat >= 0 && track.totalFrames > 0) {
                track.beatgridDownbeat = parsed.beatgridDownbeat / track.totalFrames;
            } else {
                track.beatgridDownbeat = parsed.beatgridDownbeat;
            }
            track.beatgridBpm = parsed.beatgridBpm;
            track.dateModified = new Date().toISOString();
            await db.putTrack(track);
        } else {
            console.warn('No track found for cue hash:', hash);
        }
    }
    await loadData();
    refreshLibrary();
    e.target.value = '';
}

// ---- .djlib import ----

async function handleDjlibImport(e) {
    const file = e.target.files[0];
    if (!file) return;
    const text = await file.text();
    const result = await db.importLibrary(text, true);
    alert(`Imported ${result.trackCount} tracks, ${result.playlistCount} playlists`);
    await loadData();
    refreshLibrary();
    refreshPlaylists();
    e.target.value = '';
}

// ---- Exports ----

async function exportDjlib() {
    const json = await db.exportLibrary();
    downloadFile(json, 'dj-library.djlib', 'application/json');
}

async function exportCueZip() {
    if (typeof JSZip === 'undefined') {
        // Try loading JSZip
        try {
            await loadScript('lib/jszip.min.js');
        } catch {
            alert('JSZip not available. Place jszip.min.js in web/lib/');
            return;
        }
    }
    const zip = new JSZip();
    for (const track of state.tracks) {
        const hasCues = track.cues && track.cues.some(c => c >= 0);
        const hasBeatgrid = track.beatgridBpm > 0;
        if (!hasCues && !hasBeatgrid) continue;
        // Convert normalized cues back to frame positions
        const frameCues = (track.cues || []).map(c =>
            c >= 0 ? Math.round(c * (track.totalFrames || 0)) : -1
        );
        while (frameCues.length < 8) frameCues.push(-1);
        // Convert normalized beatgrid downbeat back to frame position
        const bgDownbeat = (track.beatgridDownbeat >= 0 && track.totalFrames > 0)
            ? Math.round(track.beatgridDownbeat * track.totalFrames) : -1;
        const content = generateCueFile(frameCues, bgDownbeat, track.beatgridBpm || 0);
        zip.file(track.cueFileHash + '.cue', content);
    }
    const blob = await zip.generateAsync({ type: 'blob' });
    downloadBlob(blob, 'dj-cues.zip');
}

async function exportPlaylistTxt() {
    if (typeof JSZip === 'undefined') {
        try { await loadScript('lib/jszip.min.js'); } catch {
            alert('JSZip not available');
            return;
        }
    }
    const playlists = await db.getAllPlaylists();
    if (!playlists.length) { alert('No playlists to export'); return; }

    const zip = new JSZip();
    const playlistDir = zip.folder('dj_playlists');
    for (const pl of playlists) {
        const lines = [];
        for (const trackId of pl.trackIds) {
            const track = state.tracks.find(t => t.id === trackId);
            if (track) lines.push(track.filePath);
        }
        playlistDir.file(pl.name + '.txt', lines.join('\n') + '\n');
    }
    const blob = await zip.generateAsync({ type: 'blob' });
    downloadBlob(blob, 'dj-playlists.zip');
}

// ---- Bundle export (Music + cues + playlists in one zip) ----

async function exportBundle() {
    if (typeof JSZip === 'undefined') {
        try { await loadScript('lib/jszip.min.js'); } catch {
            alert('JSZip not available');
            return;
        }
    }

    const playlists = await db.getAllPlaylists();
    if (!playlists.length) { alert('No playlists to export'); return; }

    const folderName = prompt('Export folder name:', 'dj-set');
    if (!folderName) return;

    const zip = new JSZip();
    const root = zip.folder(folderName);
    const musicDir = root.folder('Music');
    const cueDir = root.folder('dj_cues');
    const plDir = root.folder('dj_playlists');

    // Collect all tracks referenced by any playlist
    const trackIds = new Set();
    for (const pl of playlists) {
        for (const id of pl.trackIds) trackIds.add(id);
    }

    // Add audio files + cue files for each track
    let added = 0;
    for (const id of trackIds) {
        const track = state.tracks.find(t => t.id === id);
        if (!track) continue;

        // Audio blob
        const audioData = await db.getAudioBlob(id);
        if (audioData) {
            musicDir.file(track.fileName, audioData);
        }

        // Cue file (if any cues set or beatgrid defined)
        const hasCues = track.cues && track.cues.some(c => c >= 0);
        const hasBeatgrid = track.beatgridBpm > 0;
        if (hasCues || hasBeatgrid) {
            const frameCues = (track.cues || []).map(c =>
                c >= 0 ? Math.round(c * (track.totalFrames || 0)) : -1
            );
            while (frameCues.length < 8) frameCues.push(-1);
            const bgDownbeat = (track.beatgridDownbeat >= 0 && track.totalFrames > 0)
                ? Math.round(track.beatgridDownbeat * track.totalFrames) : -1;
            cueDir.file(track.cueFileHash + '.cue', generateCueFile(frameCues, bgDownbeat, track.beatgridBpm || 0));
        }

        added++;
    }

    // Playlist .txt files
    for (const pl of playlists) {
        const lines = [];
        for (const trackId of pl.trackIds) {
            const track = state.tracks.find(t => t.id === trackId);
            if (track) lines.push(track.filePath);
        }
        plDir.file(pl.name + '.txt', lines.join('\n') + '\n');
    }

    const blob = await zip.generateAsync({ type: 'blob' });
    downloadBlob(blob, folderName + '.zip');
    alert(`Exported ${added} tracks, ${playlists.length} playlists`);
}

// ---- Settings ----

function openSettings() {
    document.getElementById('setting-lib-root').value = state.moveLibraryRoot;
    document.getElementById('settings-modal').classList.add('active');
}

function closeSettings() {
    document.getElementById('settings-modal').classList.remove('active');
}

function saveSettings() {
    state.moveLibraryRoot = document.getElementById('setting-lib-root').value;
    localStorage.setItem('moveLibraryRoot', state.moveLibraryRoot);
    closeSettings();
}

// ---- Drag & drop ----

function initDragDrop() {
    const overlay = document.getElementById('drop-overlay');
    let dragCount = 0;

    document.addEventListener('dragenter', (e) => {
        e.preventDefault();
        dragCount++;
        overlay.classList.add('active');
    });
    document.addEventListener('dragleave', (e) => {
        e.preventDefault();
        dragCount--;
        if (dragCount <= 0) { overlay.classList.remove('active'); dragCount = 0; }
    });
    document.addEventListener('dragover', (e) => e.preventDefault());
    document.addEventListener('drop', async (e) => {
        e.preventDefault();
        overlay.classList.remove('active');
        dragCount = 0;
        const files = Array.from(e.dataTransfer.files).filter(f =>
            f.type.startsWith('audio/') || /\.(wav|mp3|m4a|flac|ogg|aif|aiff)$/i.test(f.name)
        );
        for (const file of files) {
            await importAudioFile(file);
        }
        if (files.length) {
            await loadData();
            refreshLibrary();
        }
    });
}

// ---- Helpers ----

function downloadFile(content, name, type) {
    const blob = new Blob([content], { type });
    downloadBlob(blob, name);
}

function downloadBlob(blob, name) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.click();
    URL.revokeObjectURL(url);
}

function loadScript(src) {
    return new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = src;
        s.onload = resolve;
        s.onerror = reject;
        document.head.appendChild(s);
    });
}
