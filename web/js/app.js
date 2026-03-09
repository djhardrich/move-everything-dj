/*
 * DJ Library Manager — main application.
 * Init, global state, routing between views.
 */

import * as db from './db.js';
import { initLibraryView, refreshLibrary } from './ui/library.js';
import { initPlaylistSidebar, refreshPlaylists } from './ui/playlists.js';
import { initDetail } from './ui/detail.js';
import { initWaveformEditor } from './ui/waveform-editor.js';
import { initToolbar } from './ui/toolbar.js';

// ---- Global state ----

export const state = {
    tracks: [],
    playlists: [],
    selectedTrackId: null,
    selectedPlaylistId: null, // null = "All Tracks"
    searchQuery: '',
    moveLibraryRoot: '/data/UserData/UserLibrary/Samples',
    sortField: 'title',
    sortAsc: true,
};

// ---- Track selection ----

export async function selectTrack(trackId) {
    state.selectedTrackId = trackId;
    document.dispatchEvent(new CustomEvent('track-selected', { detail: { trackId } }));
}

export async function selectPlaylist(playlistId) {
    state.selectedPlaylistId = playlistId;
    document.dispatchEvent(new CustomEvent('playlist-selected', { detail: { playlistId } }));
}

// ---- Data loading ----

export async function loadData() {
    state.tracks = await db.getAllTracks();
    state.playlists = await db.getAllPlaylists();
}

// ---- Filtered / sorted track list ----

export function getVisibleTracks() {
    let tracks = state.tracks;

    // Filter by playlist
    if (state.selectedPlaylistId) {
        const pl = state.playlists.find(p => p.id === state.selectedPlaylistId);
        if (pl) {
            const idSet = new Set(pl.trackIds);
            tracks = tracks.filter(t => idSet.has(t.id));
            // Preserve playlist order
            const orderMap = new Map(pl.trackIds.map((id, i) => [id, i]));
            tracks.sort((a, b) => (orderMap.get(a.id) || 0) - (orderMap.get(b.id) || 0));
            if (!state.searchQuery) return tracks; // playlist order, no additional sort
        }
    }

    // Search filter
    if (state.searchQuery) {
        const q = state.searchQuery.toLowerCase();
        tracks = tracks.filter(t =>
            (t.title || '').toLowerCase().includes(q) ||
            (t.artist || '').toLowerCase().includes(q) ||
            (t.fileName || '').toLowerCase().includes(q) ||
            (t.comment || '').toLowerCase().includes(q)
        );
    }

    // Sort
    const field = state.sortField;
    const dir = state.sortAsc ? 1 : -1;
    tracks = [...tracks].sort((a, b) => {
        let va = a[field], vb = b[field];
        if (va == null) va = '';
        if (vb == null) vb = '';
        if (typeof va === 'string') va = va.toLowerCase();
        if (typeof vb === 'string') vb = vb.toLowerCase();
        if (va < vb) return -1 * dir;
        if (va > vb) return 1 * dir;
        return 0;
    });

    return tracks;
}

// ---- Init ----

async function init() {
    await loadData();
    initToolbar();
    initPlaylistSidebar();
    initLibraryView();
    initDetail();
    initWaveformEditor();
    refreshPlaylists();
    refreshLibrary();
}

document.addEventListener('DOMContentLoaded', init);
