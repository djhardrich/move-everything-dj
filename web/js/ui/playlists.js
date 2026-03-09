/*
 * Playlist sidebar — create, rename, delete, drag-to-add.
 */

import { state, selectPlaylist, loadData } from '../app.js';
import * as db from '../db.js';
import { refreshLibrary } from './library.js';

export function initPlaylistSidebar() {
    document.getElementById('btn-new-playlist').addEventListener('click', createPlaylist);

    // Close mobile sidebar when clicking outside
    document.addEventListener('click', (e) => {
        const sidebar = document.getElementById('sidebar');
        if (sidebar.classList.contains('mobile-open') &&
            !sidebar.contains(e.target) &&
            e.target.id !== 'btn-sidebar-toggle') {
            sidebar.classList.remove('mobile-open');
        }
    });
}

export function refreshPlaylists() {
    const list = document.getElementById('playlist-list');
    list.innerHTML = '';

    // "All Tracks" item
    const allItem = document.createElement('div');
    allItem.className = 'playlist-item' + (state.selectedPlaylistId === null ? ' selected' : '');
    allItem.innerHTML = `<span>All Tracks</span><span class="count">${state.tracks.length}</span>`;
    allItem.addEventListener('click', () => {
        selectPlaylist(null);
        refreshPlaylists();
        refreshLibrary();
    });
    list.appendChild(allItem);

    // Playlists
    state.playlists.forEach(pl => {
        const item = document.createElement('div');
        item.className = 'playlist-item' + (state.selectedPlaylistId === pl.id ? ' selected' : '');
        item.innerHTML = `<span>${esc(pl.name)}</span><span class="count">${pl.trackIds.length}</span>`;

        item.addEventListener('click', () => {
            selectPlaylist(pl.id);
            refreshPlaylists();
            refreshLibrary();
        });

        item.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            showPlaylistMenu(pl, e);
        });

        // Drop tracks onto playlist
        item.addEventListener('dragover', (e) => {
            e.preventDefault();
            item.style.background = 'var(--bg-hover)';
        });
        item.addEventListener('dragleave', () => {
            item.style.background = '';
        });
        item.addEventListener('drop', async (e) => {
            e.preventDefault();
            item.style.background = '';
            const trackId = e.dataTransfer.getData('text/plain');
            if (trackId && !pl.trackIds.includes(trackId)) {
                pl.trackIds.push(trackId);
                pl.dateModified = new Date().toISOString();
                await db.putPlaylist(pl);
                await loadData();
                refreshPlaylists();
                refreshLibrary();
            }
        });

        list.appendChild(item);
    });
}

async function createPlaylist() {
    const name = prompt('Playlist name:');
    if (!name) return;
    const pl = {
        id: db.uuid(),
        name,
        trackIds: [],
        dateCreated: new Date().toISOString(),
        dateModified: new Date().toISOString(),
    };
    await db.putPlaylist(pl);
    await loadData();
    refreshPlaylists();
}

function showPlaylistMenu(pl, e) {
    // Simple context menu via prompt
    const action = prompt(`Playlist: ${pl.name}\nType "rename" or "delete":`);
    if (!action) return;
    if (action.toLowerCase() === 'rename') {
        renamePlaylist(pl);
    } else if (action.toLowerCase() === 'delete') {
        deletePlaylist(pl);
    }
}

async function renamePlaylist(pl) {
    const name = prompt('New name:', pl.name);
    if (!name) return;
    pl.name = name;
    pl.dateModified = new Date().toISOString();
    await db.putPlaylist(pl);
    await loadData();
    refreshPlaylists();
}

async function deletePlaylist(pl) {
    if (!confirm(`Delete playlist "${pl.name}"?`)) return;
    await db.deletePlaylist(pl.id);
    if (state.selectedPlaylistId === pl.id) selectPlaylist(null);
    await loadData();
    refreshPlaylists();
    refreshLibrary();
}

function esc(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}
