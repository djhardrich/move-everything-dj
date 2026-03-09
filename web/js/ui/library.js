/*
 * Library view — track table with sort, search, filter.
 */

import { state, selectTrack, getVisibleTracks } from '../app.js';
import { formatTimeShort } from '../cue-io.js';

export function initLibraryView() {
    // Sort headers
    document.querySelectorAll('#track-table th[data-field]').forEach(th => {
        th.addEventListener('click', () => {
            const field = th.dataset.field;
            if (field === 'index') return;
            if (state.sortField === field) {
                state.sortAsc = !state.sortAsc;
            } else {
                state.sortField = field;
                state.sortAsc = true;
            }
            refreshLibrary();
        });
    });
}

export function refreshLibrary() {
    const tbody = document.getElementById('track-tbody');
    const emptyState = document.getElementById('empty-state');
    const tracks = getVisibleTracks();

    // Update sort arrows
    document.querySelectorAll('#track-table th[data-field]').forEach(th => {
        th.classList.toggle('sorted', th.dataset.field === state.sortField);
        const existing = th.querySelector('.sort-arrow');
        if (existing) existing.remove();
        if (th.dataset.field === state.sortField) {
            const arrow = document.createElement('span');
            arrow.className = 'sort-arrow';
            arrow.textContent = state.sortAsc ? '\u25B2' : '\u25BC';
            th.appendChild(arrow);
        }
    });

    if (tracks.length === 0 && state.tracks.length === 0) {
        tbody.innerHTML = '';
        emptyState.style.display = 'flex';
        return;
    }
    emptyState.style.display = 'none';

    tbody.innerHTML = '';
    tracks.forEach((track, i) => {
        const tr = document.createElement('tr');
        tr.dataset.trackId = track.id;
        if (track.id === state.selectedTrackId) tr.classList.add('selected');

        tr.innerHTML = `
            <td>${i + 1}</td>
            <td>${esc(track.title || track.fileName)}</td>
            <td>${esc(track.artist || '')}</td>
            <td>${track.bpm ? track.bpm.toFixed(1) : ''}</td>
            <td>${esc(track.key || '')}</td>
            <td>${track.duration ? formatTimeShort(track.duration) : ''}</td>
        `;

        tr.addEventListener('click', () => selectTrack(track.id));

        // Drag for playlist reorder
        tr.draggable = true;
        tr.addEventListener('dragstart', (e) => {
            e.dataTransfer.setData('text/plain', track.id);
            e.dataTransfer.effectAllowed = 'move';
        });

        tbody.appendChild(tr);
    });
}

function esc(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}
