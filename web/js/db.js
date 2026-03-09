/*
 * IndexedDB wrapper + JSON import/export.
 * Stores: tracks (metadata), audio (blobs), playlists.
 */

const DB_NAME = 'dj-library';
const DB_VERSION = 1;

let db = null;

function openDB() {
    return new Promise((resolve, reject) => {
        if (db) { resolve(db); return; }
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = (e) => {
            const d = e.target.result;
            if (!d.objectStoreNames.contains('tracks')) {
                const ts = d.createObjectStore('tracks', { keyPath: 'id' });
                ts.createIndex('fileName', 'fileName', { unique: false });
                ts.createIndex('artist', 'artist', { unique: false });
                ts.createIndex('title', 'title', { unique: false });
                ts.createIndex('dateAdded', 'dateAdded', { unique: false });
            }
            if (!d.objectStoreNames.contains('audio')) {
                d.createObjectStore('audio', { keyPath: 'id' });
            }
            if (!d.objectStoreNames.contains('playlists')) {
                d.createObjectStore('playlists', { keyPath: 'id' });
            }
        };
        req.onsuccess = (e) => { db = e.target.result; resolve(db); };
        req.onerror = (e) => reject(e.target.error);
    });
}

function tx(storeName, mode = 'readonly') {
    return db.transaction(storeName, mode).objectStore(storeName);
}

function reqToPromise(req) {
    return new Promise((resolve, reject) => {
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

// ---- Tracks ----

export async function getAllTracks() {
    await openDB();
    return reqToPromise(tx('tracks').getAll());
}

export async function getTrack(id) {
    await openDB();
    return reqToPromise(tx('tracks').get(id));
}

export async function putTrack(track) {
    await openDB();
    return reqToPromise(tx('tracks', 'readwrite').put(track));
}

export async function deleteTrack(id) {
    await openDB();
    const store = tx('tracks', 'readwrite');
    return reqToPromise(store.delete(id));
}

// ---- Audio blobs ----

export async function getAudioBlob(id) {
    await openDB();
    const rec = await reqToPromise(tx('audio').get(id));
    return rec ? rec.data : null;
}

export async function putAudioBlob(id, arrayBuffer) {
    await openDB();
    return reqToPromise(tx('audio', 'readwrite').put({ id, data: arrayBuffer }));
}

export async function deleteAudioBlob(id) {
    await openDB();
    return reqToPromise(tx('audio', 'readwrite').delete(id));
}

// ---- Playlists ----

export async function getAllPlaylists() {
    await openDB();
    return reqToPromise(tx('playlists').getAll());
}

export async function getPlaylist(id) {
    await openDB();
    return reqToPromise(tx('playlists').get(id));
}

export async function putPlaylist(playlist) {
    await openDB();
    return reqToPromise(tx('playlists', 'readwrite').put(playlist));
}

export async function deletePlaylist(id) {
    await openDB();
    return reqToPromise(tx('playlists', 'readwrite').delete(id));
}

// ---- Import / Export (.djlib) ----

export async function exportLibrary() {
    const tracks = await getAllTracks();
    const playlists = await getAllPlaylists();
    // Strip waveformPeaks from export
    const exportTracks = tracks.map(t => {
        const { waveformPeaks, ...rest } = t;
        return rest;
    });
    return JSON.stringify({ version: 1, tracks: exportTracks, playlists }, null, 2);
}

export async function importLibrary(jsonStr, merge = true) {
    const data = JSON.parse(jsonStr);
    if (data.version !== 1) throw new Error('Unsupported .djlib version: ' + data.version);

    await openDB();

    if (data.tracks) {
        for (const track of data.tracks) {
            if (merge) {
                const existing = await getTrack(track.id);
                if (existing) {
                    // Merge: keep existing waveformPeaks and audio
                    track.waveformPeaks = existing.waveformPeaks;
                }
            }
            await putTrack(track);
        }
    }

    if (data.playlists) {
        for (const pl of data.playlists) {
            await putPlaylist(pl);
        }
    }

    return { trackCount: data.tracks?.length || 0, playlistCount: data.playlists?.length || 0 };
}

// ---- UUID ----

export function uuid() {
    return crypto.randomUUID();
}
