/*
 * dj_plugin.cpp - Dual-Deck DJ DSP plugin for Move Anything
 * Copyright (c) 2026 DJ Hard Rich
 * Licensed under CC BY-NC-SA 4.0
 *
 * Two independent decks (A & B) each with:
 *   - 4-track stem player or MOD channel decomposition
 *   - Bungee timestretch and pitch-shift
 *   - 8 hot cues, stutter FX, loop
 *
 * Decks are mixed through a crossfader with per-deck volume.
 * Plugin API v2 implementation.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <pthread.h>
#include <atomic>
#include <dirent.h>
#include <sys/stat.h>

#include <bungee/Bungee.h>
#include <submodules/pffft/pffft.h>

#ifdef HAS_MP3
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>
#endif

#ifdef HAS_M4A
#include "aacdecoder_lib.h"
#endif

#ifdef HAS_FLAC
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#endif

#ifdef HAS_LIBXMP
#include <xmp.h>
#endif

/* ------------------------------------------------------------------ */
/*  Embedded host/plugin API structs                                  */
/* ------------------------------------------------------------------ */

extern "C" {

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);

} // extern "C"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

static const int    SAMPLE_RATE     = 44100;
static const int    MAX_FRAMES      = SAMPLE_RATE * 300;
static const int    NUM_STEMS       = 4;
static const int    NUM_CUES        = 8;
static const int    NUM_DECKS       = 2;
static const int    OUT_BUF_CAPACITY = 16384;
static const int    RENDER_BUF_SIZE = 2048; /* max frames_per_block * 2 channels, generous */

static const int    NUM_STUTTER_SIZES = 8;
static const float  STUTTER_BEATS[NUM_STUTTER_SIZES] = {
    0.03125f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f
};

static const int    NUM_LOOP_SIZES = 13;
static const float  LOOP_BEATS[NUM_LOOP_SIZES] = {
    0.015625f, 0.03125f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f,
    2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f
};

/* ------------------------------------------------------------------ */
/*  Track state                                                       */
/* ------------------------------------------------------------------ */

struct track_t {
    char file_path[512];
    char file_name[128];
    float *audio_data;
    int    audio_frames;
    float  duration_secs;
    int    muted;
    float  volume;
};

/* ------------------------------------------------------------------ */
/*  Deck state (one per deck)                                         */
/* ------------------------------------------------------------------ */

struct deck_t {
    char loaded_file[512];
    int  is_mod;

    track_t tracks[NUM_STEMS];
    int max_frames;

    int    playing;
    int    pitch_semitones;
    int    speed_pct;
    float  speed;
    float  bpm;
    float  detected_bpm;
    int    vinyl_speed_pct;

    double cue_frames[NUM_CUES];

    /* Beat grid: downbeat position + exact BPM */
    double beatgrid_downbeat;   /* frame position of first downbeat (-1 = not set) */
    float  beatgrid_bpm;        /* exact BPM to 0.01 precision (0 = not set) */

    volatile int stutter_active;  /* volatile: read by audio thread, written by UI thread */
    double stutter_start;         /* frame position of stutter loop start */
    int    stutter_size_frames;
    double stutter_pos;           /* current read position within stutter (fractional frame) */
    int    stutter_repeat;        /* which repeat we're on (0, 1, 2...) */
    uint32_t stutter_rng;         /* xorshift PRNG state for reverse decisions */
    int    stutter_reversed;      /* 1 = current repeat plays backwards */
    float  stutter_rand_pitch;    /* random pitch interval for current repeat (semitones) */
    float  stutter_pitch_semis;    /* stutter pitch shift in semitones (-12 to +12) */
    int    stutter_filter_pos;    /* 0=heavy LPF, 50=bypass, 100=heavy HPF */
    float  stutter_filter_z1;     /* one-pole LPF state for stutter filter */
    float  stutter_filter_z2;     /* second pole for steeper rolloff */

    volatile int loop_active;     /* volatile: read by audio thread, written by UI thread */
    double loop_start;
    int    loop_size_frames;
    int    loop_beats_idx;

    int    slip_mode;       /* 0=off, 1=on */
    double slip_position;   /* shadow position that always advances */
    int    slip_engaged;    /* currently in a slip action (loop/cue) */

    int    cues_dirty;      /* deferred cue save flag */

    Bungee::Stretcher<Bungee::Basic> *stretcher;
    Bungee::Request req;
    float *grain_input;
    int    max_grain;

    float *out_buf;
    int    out_head;        /* read position in ring buffer */
    int    out_count;       /* number of valid frames in ring */

    /* DJ filter: 0=full LPF, 50=bypass, 100=full HPF */
    int    filter_pos;    /* 0-100, default 50 */
    float  flt_lp_l, flt_lp_r;  /* LPF state */
    float  flt_hp_l, flt_hp_r;  /* HPF state (prev sample) */

    /* Background loading: new track data staged here, then swapped in */
    std::atomic<int> load_state;  /* 0=idle, 1=loading, 2=ready */
    track_t new_tracks[NUM_STEMS];
    int    new_is_mod;
    int    new_max_frames;
    float  new_detected_bpm;
    char   new_loaded_file[512];
    double new_cue_frames[NUM_CUES];
    double new_beatgrid_downbeat;
    float  new_beatgrid_bpm;
    int    new_stem_slot;  /* which slot was loaded (-1 = full load) */

    /* Waveform overview: 128 peak values (0-15) for display */
    uint8_t waveform[128];
    uint8_t new_waveform[128];
};

/* ------------------------------------------------------------------ */
/*  Instance state (global)                                           */
/* ------------------------------------------------------------------ */

struct load_request_t {
    deck_t *dk;
    char path[512];
    int stem_slot;
    /* Snapshot of existing stem state taken on the calling thread (safe) */
    float stem_volumes[NUM_STEMS];
    int   stem_muted[NUM_STEMS];
    char  stem_file_names[NUM_STEMS][128];
    char  stem_file_paths[NUM_STEMS][512];
    int   stem_audio_frames[NUM_STEMS];
    float *stem_audio_copies[NUM_STEMS]; /* heap-copied audio for non-target stems */
    int   was_mod;
};

/* Deferred free list: audio buffers freed outside render_block */
static const int MAX_DEFERRED_FREES = 16;
static void *s_deferred_frees[MAX_DEFERRED_FREES];
static std::atomic<int> s_deferred_free_count{0};

static void deferred_free(void *p) {
    if (!p) return;
    int idx = s_deferred_free_count.load(std::memory_order_relaxed);
    if (idx < MAX_DEFERRED_FREES) {
        s_deferred_frees[idx] = p;
        s_deferred_free_count.store(idx + 1, std::memory_order_release);
    } else {
        /* Fallback: free immediately (shouldn't happen in practice) */
        free(p);
    }
}

static void flush_deferred_frees() {
    int count = s_deferred_free_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; i++) {
        free(s_deferred_frees[i]);
        s_deferred_frees[i] = nullptr;
    }
    s_deferred_free_count.store(0, std::memory_order_release);
}

static bool s_cue_dir_created = false;

/* Forward declarations needed by render worker */
static void host_log(const char *fmt, ...);
static void render_deck(deck_t *dk, float *out_lr, int frames);
static void free_playlists();

/* ------------------------------------------------------------------ */
/*  Persistent render worker thread (deck B on separate core)         */
/* ------------------------------------------------------------------ */

struct render_worker_t {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond_work;   /* main → worker: work available */
    pthread_cond_t  cond_done;   /* worker → main: render complete */
    deck_t         *dk;
    float          *out_buf;
    int             frames;
    int             has_work;    /* 1 = render job pending */
    int             done;        /* 1 = render job complete */
    int             shutdown;    /* 1 = exit thread */
};

static void *render_worker_func(void *arg) {
    render_worker_t *w = (render_worker_t *)arg;
    pthread_mutex_lock(&w->mutex);
    for (;;) {
        while (!w->has_work && !w->shutdown)
            pthread_cond_wait(&w->cond_work, &w->mutex);
        if (w->shutdown) break;
        /* Grab work params */
        deck_t *dk = w->dk;
        float  *buf = w->out_buf;
        int     frames = w->frames;
        w->has_work = 0;
        pthread_mutex_unlock(&w->mutex);

        /* Do the work — render_deck is self-contained per deck */
        render_deck(dk, buf, frames);

        pthread_mutex_lock(&w->mutex);
        w->done = 1;
        pthread_cond_signal(&w->cond_done);
    }
    pthread_mutex_unlock(&w->mutex);
    return nullptr;
}

static render_worker_t *create_render_worker() {
    render_worker_t *w = (render_worker_t *)calloc(1, sizeof(render_worker_t));
    if (!w) return nullptr;
    pthread_mutex_init(&w->mutex, nullptr);
    pthread_cond_init(&w->cond_work, nullptr);
    pthread_cond_init(&w->cond_done, nullptr);
    w->has_work = 0;
    w->done = 0;
    w->shutdown = 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024); /* 128K stack, plenty for render */
    int rc = pthread_create(&w->thread, &attr, render_worker_func, w);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        host_log("[dj] failed to create render worker thread: %d", rc);
        pthread_mutex_destroy(&w->mutex);
        pthread_cond_destroy(&w->cond_work);
        pthread_cond_destroy(&w->cond_done);
        free(w);
        return nullptr;
    }
    host_log("[dj] render worker thread started");
    return w;
}

static void destroy_render_worker(render_worker_t *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    w->shutdown = 1;
    pthread_cond_signal(&w->cond_work);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, nullptr);
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond_work);
    pthread_cond_destroy(&w->cond_done);
    free(w);
}

struct instance_t {
    char module_dir[512];
    deck_t decks[NUM_DECKS]; /* 0=A, 1=B */

    float  master_vol;
    int    crossfader;      /* 0=full A, 50=center, 100=full B */
    float  deck_vol[NUM_DECKS];

    float *render_buf_a;    /* cache-line aligned */
    float *render_buf_b;    /* cache-line aligned */

    render_worker_t *worker; /* persistent thread for deck B rendering */
};

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */

static const host_api_v1_t *s_host = nullptr;

/* ------------------------------------------------------------------ */
/*  Utility helpers                                                   */
/* ------------------------------------------------------------------ */

static FILE *s_logfile = nullptr;

static void host_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_host && s_host->log) s_host->log(buf);
    if (!s_logfile)
        s_logfile = fopen("/data/UserData/move-anything/dj_debug.log", "w");
    if (s_logfile) { fprintf(s_logfile, "%s\n", buf); fflush(s_logfile); }
}

static const char *basename_ptr(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Forward declarations */
static void recalc_max_frames(deck_t *dk);
static void reset_stretcher(deck_t *dk, double position);

/* ------------------------------------------------------------------ */
/*  Resample stereo interleaved float buffer to SAMPLE_RATE            */
/* ------------------------------------------------------------------ */

static float *resample_to_target(float *buf, int src_frames, int src_rate, int *out_frames) {
    if (src_rate == SAMPLE_RATE || src_rate <= 0) { *out_frames = src_frames; return buf; }
    double ratio = (double)SAMPLE_RATE / (double)src_rate;
    int dst_frames = (int)(src_frames * ratio);
    if (dst_frames <= 0 || dst_frames > MAX_FRAMES) { *out_frames = src_frames; return buf; }
    float *dst = (float *)calloc(dst_frames * 2, sizeof(float));
    if (!dst) { *out_frames = src_frames; return buf; }
    for (int i = 0; i < dst_frames; i++) {
        double srcPos = i / ratio;
        int s0 = (int)srcPos;
        double frac = srcPos - s0;
        int s1 = s0 + 1;
        if (s1 >= src_frames) s1 = src_frames - 1;
        dst[i*2+0] = (float)(buf[s0*2+0] * (1.0 - frac) + buf[s1*2+0] * frac);
        dst[i*2+1] = (float)(buf[s0*2+1] * (1.0 - frac) + buf[s1*2+1] * frac);
    }
    free(buf);
    *out_frames = dst_frames;
    return dst;
}

/* ------------------------------------------------------------------ */
/*  WAV loader                                                        */
/* ------------------------------------------------------------------ */

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool load_wav(track_t *trk, const char *path) {
    if (trk->audio_data) { free(trk->audio_data); trk->audio_data = nullptr; trk->audio_frames = 0; trk->duration_secs = 0; }
    FILE *fp = fopen(path, "rb");
    if (!fp) { host_log("[dj] cannot open %s", path); return false; }
    fseek(fp, 0, SEEK_END); long file_size = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (file_size < 44) { fclose(fp); return false; }
    uint8_t *raw = (uint8_t *)malloc(file_size);
    if (!raw) { fclose(fp); return false; }
    size_t read_bytes = fread(raw, 1, file_size, fp); fclose(fp);
    if ((long)read_bytes != file_size) { free(raw); return false; }
    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) { free(raw); return false; }

    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0;
    const uint8_t *data_ptr = nullptr;
    bool found_fmt = false, found_data = false;
    size_t pos = 12;
    while (pos + 8 <= (size_t)file_size) {
        const uint8_t *chunk = raw + pos;
        uint32_t chunk_size = read_u32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = read_u16(chunk + 8); num_channels = read_u16(chunk + 10);
            sample_rate = read_u32(chunk + 12); bits_per_sample = read_u16(chunk + 22);
            found_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_ptr = chunk + 8; data_size = chunk_size; found_data = true;
        }
        pos += 8 + chunk_size; if (chunk_size & 1) pos++;
        if (found_fmt && found_data) break;
    }
    if (!found_fmt || !found_data) { free(raw); return false; }

    bool is_pcm = (audio_format == 1), is_float = (audio_format == 3);
    if ((!is_pcm && !is_float) || num_channels < 1 || num_channels > 2) { free(raw); return false; }
    if (is_pcm && bits_per_sample != 16 && bits_per_sample != 24) { free(raw); return false; }
    if (is_float && bits_per_sample != 32) { free(raw); return false; }

    int bytes_per_sample = bits_per_sample / 8;
    int block_align = bytes_per_sample * num_channels;
    int total_frames = (int)(data_size / block_align);
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;
    if (total_frames <= 0) { free(raw); return false; }

    float *buf = (float *)calloc(total_frames * 2, sizeof(float));
    if (!buf) { free(raw); return false; }
    const uint8_t *src = data_ptr;
    for (int i = 0; i < total_frames; i++) {
        float samples[2] = {0, 0};
        for (int ch = 0; ch < num_channels; ch++) {
            float val = 0;
            if (is_pcm && bits_per_sample == 16) { val = (int16_t)read_u16(src) / 32768.0f; src += 2; }
            else if (is_pcm && bits_per_sample == 24) {
                int32_t s = (int32_t)src[0] | ((int32_t)src[1]<<8) | ((int32_t)src[2]<<16);
                if (s & 0x800000) s |= (int32_t)0xFF000000; val = s / 8388608.0f; src += 3;
            } else if (is_float) { uint32_t u = read_u32(src); memcpy(&val, &u, sizeof(float)); src += 4; }
            samples[ch] = val;
        }
        buf[i*2+0] = samples[0]; buf[i*2+1] = (num_channels == 1) ? samples[0] : samples[1];
    }
    free(raw);
    float duration_secs = (float)total_frames / (float)sample_rate;
    buf = resample_to_target(buf, total_frames, sample_rate, &total_frames);
    trk->audio_data = buf; trk->audio_frames = total_frames;
    trk->duration_secs = duration_secs;
    snprintf(trk->file_name, sizeof(trk->file_name), "%s", basename_ptr(path));
    snprintf(trk->file_path, sizeof(trk->file_path), "%s", path);
    host_log("[dj] loaded wav: %s (%d frames, %.1fs, src %dHz)", trk->file_name, total_frames, trk->duration_secs, sample_rate);
    return true;
}

/* ------------------------------------------------------------------ */
/*  MOD loader                                                        */
/* ------------------------------------------------------------------ */

#ifdef HAS_LIBXMP

static bool load_mod(deck_t *dk, const char *path) {
    xmp_context ctx = xmp_create_context();
    if (!ctx) return false;
    if (xmp_load_module(ctx, path) < 0) { xmp_free_context(ctx); return false; }

    struct xmp_module_info mi;
    xmp_get_module_info(ctx, &mi);
    int mod_channels = mi.mod->chn;
    int stems = (mod_channels < NUM_STEMS) ? mod_channels : NUM_STEMS;

    for (int t = 0; t < NUM_STEMS; t++) {
        if (dk->tracks[t].audio_data) { free(dk->tracks[t].audio_data); dk->tracks[t].audio_data = nullptr; }
        dk->tracks[t].audio_frames = 0; dk->tracks[t].duration_secs = 0;
        dk->tracks[t].file_name[0] = '\0'; dk->tracks[t].file_path[0] = '\0';
        dk->tracks[t].muted = 0; dk->tracks[t].volume = 1.0f;
    }

    const char *fname = basename_ptr(path);
    for (int ch = 0; ch < stems; ch++) {
        if (xmp_start_player(ctx, SAMPLE_RATE, 0) < 0) continue;
        for (int i = 0; i < mod_channels; i++) xmp_channel_mute(ctx, i, (i == ch) ? 0 : 1);
        xmp_restart_module(ctx);

        float *buf = (float *)calloc(MAX_FRAMES * 2, sizeof(float));
        if (!buf) { xmp_end_player(ctx); continue; }
        int total_frames = 0;
        int16_t render_buf[4096];
        while (total_frames < MAX_FRAMES) {
            int ret = xmp_play_buffer(ctx, render_buf, sizeof(render_buf), 0);
            if (ret < 0) break;
            int chunk_frames = (int)(sizeof(render_buf) / 4);
            int to_copy = std::min(chunk_frames, MAX_FRAMES - total_frames);
            for (int i = 0; i < to_copy; i++) {
                buf[(total_frames+i)*2+0] = render_buf[i*2+0] / 32768.0f;
                buf[(total_frames+i)*2+1] = render_buf[i*2+1] / 32768.0f;
            }
            total_frames += to_copy;
        }
        xmp_end_player(ctx);

        if (total_frames > 0) {
            float *trimmed = (float *)realloc(buf, total_frames * 2 * sizeof(float));
            dk->tracks[ch].audio_data = trimmed ? trimmed : buf;
            dk->tracks[ch].audio_frames = total_frames;
            dk->tracks[ch].duration_secs = (float)total_frames / (float)SAMPLE_RATE;
            snprintf(dk->tracks[ch].file_name, sizeof(dk->tracks[ch].file_name), "%s Ch%d", fname, ch+1);
            snprintf(dk->tracks[ch].file_path, sizeof(dk->tracks[ch].file_path), "%s", path);
        } else { free(buf); }
    }

    if (mi.mod->bpm > 0) dk->bpm = (float)mi.mod->bpm;
    xmp_release_module(ctx); xmp_free_context(ctx);
    snprintf(dk->loaded_file, sizeof(dk->loaded_file), "%s", path);
    dk->is_mod = 1;
    recalc_max_frames(dk);
    return true;
}

#endif

/* ------------------------------------------------------------------ */
/*  MP3 loader                                                        */
/* ------------------------------------------------------------------ */

#ifdef HAS_MP3

static bool load_mp3(track_t *trk, const char *path) {
    if (trk->audio_data) { free(trk->audio_data); trk->audio_data = nullptr; trk->audio_frames = 0; trk->duration_secs = 0; }
    mp3dec_t mp3d; mp3dec_file_info_t info; memset(&info, 0, sizeof(info));
    if (mp3dec_load(&mp3d, path, &info, NULL, NULL) != 0) { host_log("[dj] mp3 decode failed: %s", path); return false; }
    if (info.samples == 0 || !info.buffer) { if (info.buffer) free(info.buffer); return false; }
    int channels = info.channels;
    int total_frames = (int)(info.samples / channels);
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;
    float *buf = (float *)calloc(total_frames * 2, sizeof(float));
    if (!buf) { free(info.buffer); return false; }
    for (int i = 0; i < total_frames; i++) {
        float L = info.buffer[i*channels] / 32768.0f;
        float R = (channels > 1) ? info.buffer[i*channels+1] / 32768.0f : L;
        buf[i*2+0] = L; buf[i*2+1] = R;
    }
    free(info.buffer);
    float duration_secs = (float)total_frames / (float)info.hz;
    buf = resample_to_target(buf, total_frames, info.hz, &total_frames);
    trk->audio_data = buf; trk->audio_frames = total_frames;
    trk->duration_secs = duration_secs;
    snprintf(trk->file_name, sizeof(trk->file_name), "%s", basename_ptr(path));
    snprintf(trk->file_path, sizeof(trk->file_path), "%s", path);
    host_log("[dj] loaded mp3: %s (%d frames, %.1fs, src %dHz)", trk->file_name, total_frames, trk->duration_secs, info.hz);
    return true;
}

#endif

/* ------------------------------------------------------------------ */
/*  M4A/AAC loader                                                    */
/* ------------------------------------------------------------------ */

#ifdef HAS_M4A

static uint32_t mp4_read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static uint16_t mp4_read_u16_be(const uint8_t *p) {
    return ((uint16_t)p[0]<<8)|(uint16_t)p[1];
}

struct mp4_sample_table {
    uint32_t *sample_sizes; int num_samples;
    uint64_t *chunk_offsets; int num_chunks;
    struct stsc_entry { uint32_t first_chunk, samples_per_chunk, desc_idx; };
    stsc_entry *stsc_entries; int num_stsc;
    uint8_t asc[64]; int asc_len;
};

static void mp4_parse_atoms(const uint8_t *data, size_t len, size_t base_offset,
                            mp4_sample_table *st, int depth) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t size32 = mp4_read_u32_be(data+pos);
        uint32_t type = mp4_read_u32_be(data+pos+4);
        uint64_t atom_size = size32; size_t header_size = 8;
        if (size32 == 1 && pos+16 <= len) {
            atom_size = ((uint64_t)mp4_read_u32_be(data+pos+8)<<32)|mp4_read_u32_be(data+pos+12);
            header_size = 16;
        }
        if (atom_size < header_size || pos+atom_size > len) break;
        const uint8_t *body = data+pos+header_size;
        size_t body_len = (size_t)(atom_size - header_size);

        if (type==0x6D6F6F76||type==0x7472616B||type==0x6D646961||type==0x6D696E66||type==0x7374626C||type==0x75647461)
            mp4_parse_atoms(body, body_len, base_offset+pos+header_size, st, depth+1);
        else if (type==0x73747364 && body_len>16) {
            const uint8_t *stsd_body = body+8; size_t stsd_len = body_len-8;
            if (stsd_len > 36) {
                uint32_t entry_size = mp4_read_u32_be(stsd_body);
                if (entry_size > 36 && stsd_len >= entry_size)
                    mp4_parse_atoms(stsd_body+28, entry_size-28, base_offset+pos+header_size+8+28, st, depth+1);
            }
        }
        else if (type==0x65736473 && body_len>12) {
            for (size_t i = 4; i+2 < body_len; i++) {
                if (body[i] == 0x05) {
                    size_t j = i+1; uint32_t dsi_len = 0;
                    while (j < body_len && (body[j]&0x80)) { dsi_len = (dsi_len<<7)|(body[j]&0x7F); j++; }
                    if (j < body_len) { dsi_len = (dsi_len<<7)|(body[j]&0x7F); j++; }
                    if (j+dsi_len <= body_len && dsi_len < sizeof(st->asc)) { memcpy(st->asc, body+j, dsi_len); st->asc_len = (int)dsi_len; }
                    break;
                }
            }
        }
        else if (type==0x7374737A && body_len>=12) {
            uint32_t uniform = mp4_read_u32_be(body+4), count = mp4_read_u32_be(body+8);
            if (count > 0 && count < 10000000) {
                st->num_samples = (int)count; st->sample_sizes = (uint32_t*)calloc(count, sizeof(uint32_t));
                if (uniform > 0) { for (uint32_t i=0;i<count;i++) st->sample_sizes[i]=uniform; }
                else if (body_len >= 12+count*4) { for (uint32_t i=0;i<count;i++) st->sample_sizes[i]=mp4_read_u32_be(body+12+i*4); }
            }
        }
        else if (type==0x7374636F && body_len>=8) {
            uint32_t count = mp4_read_u32_be(body+4);
            if (count>0 && count<10000000 && body_len>=8+count*4) {
                st->num_chunks=(int)count; st->chunk_offsets=(uint64_t*)calloc(count,sizeof(uint64_t));
                for (uint32_t i=0;i<count;i++) st->chunk_offsets[i]=mp4_read_u32_be(body+8+i*4);
            }
        }
        else if (type==0x636F3634 && body_len>=8) {
            uint32_t count = mp4_read_u32_be(body+4);
            if (count>0 && count<10000000 && body_len>=8+count*8) {
                st->num_chunks=(int)count; st->chunk_offsets=(uint64_t*)calloc(count,sizeof(uint64_t));
                for (uint32_t i=0;i<count;i++)
                    st->chunk_offsets[i]=((uint64_t)mp4_read_u32_be(body+8+i*8)<<32)|mp4_read_u32_be(body+8+i*8+4);
            }
        }
        else if (type==0x73747363 && body_len>=8) {
            uint32_t count = mp4_read_u32_be(body+4);
            if (count>0 && count<10000000 && body_len>=8+count*12) {
                st->num_stsc=(int)count;
                st->stsc_entries=(mp4_sample_table::stsc_entry*)calloc(count,sizeof(mp4_sample_table::stsc_entry));
                for (uint32_t i=0;i<count;i++) {
                    st->stsc_entries[i].first_chunk=mp4_read_u32_be(body+8+i*12);
                    st->stsc_entries[i].samples_per_chunk=mp4_read_u32_be(body+8+i*12+4);
                    st->stsc_entries[i].desc_idx=mp4_read_u32_be(body+8+i*12+8);
                }
            }
        }
        pos += (size_t)atom_size;
    }
}

static bool load_m4a(track_t *trk, const char *path) {
    if (trk->audio_data) { free(trk->audio_data); trk->audio_data=nullptr; trk->audio_frames=0; trk->duration_secs=0; }
    FILE *fp = fopen(path, "rb"); if (!fp) return false;
    fseek(fp,0,SEEK_END); long file_size=ftell(fp); fseek(fp,0,SEEK_SET);
    if (file_size<32||file_size>500*1024*1024) { fclose(fp); return false; }
    uint8_t *raw=(uint8_t*)malloc(file_size);
    if (!raw) { fclose(fp); return false; }
    if ((long)fread(raw,1,file_size,fp)!=file_size) { free(raw); fclose(fp); return false; }
    fclose(fp);

    mp4_sample_table st; memset(&st,0,sizeof(st));
    mp4_parse_atoms(raw,(size_t)file_size,0,&st,0);
    if (st.num_samples==0||st.num_chunks==0||st.asc_len==0) {
        if (st.sample_sizes) free(st.sample_sizes);
        if (st.chunk_offsets) free(st.chunk_offsets);
        if (st.stsc_entries) free(st.stsc_entries);
        free(raw); return false;
    }

    uint64_t *sample_offsets=(uint64_t*)calloc(st.num_samples,sizeof(uint64_t));
    int sample_idx=0;
    for (int chunk=0; chunk<st.num_chunks && sample_idx<st.num_samples; chunk++) {
        uint32_t spc=1;
        for (int s=0;s<st.num_stsc;s++) if ((int)(st.stsc_entries[s].first_chunk-1)<=chunk) spc=st.stsc_entries[s].samples_per_chunk;
        uint64_t offset=st.chunk_offsets[chunk];
        for (uint32_t s=0;s<spc&&sample_idx<st.num_samples;s++) { sample_offsets[sample_idx]=offset; offset+=st.sample_sizes[sample_idx]; sample_idx++; }
    }

    HANDLE_AACDECODER aac=aacDecoder_Open(TT_MP4_RAW,1);
    if (!aac) { free(sample_offsets); if(st.sample_sizes)free(st.sample_sizes); if(st.chunk_offsets)free(st.chunk_offsets); if(st.stsc_entries)free(st.stsc_entries); free(raw); return false; }
    UCHAR *asc_buf=st.asc; UINT asc_size=(UINT)st.asc_len;
    if (aacDecoder_ConfigRaw(aac,&asc_buf,&asc_size)!=AAC_DEC_OK) { aacDecoder_Close(aac); free(sample_offsets); if(st.sample_sizes)free(st.sample_sizes); if(st.chunk_offsets)free(st.chunk_offsets); if(st.stsc_entries)free(st.stsc_entries); free(raw); return false; }

    float *buf=(float*)calloc(MAX_FRAMES*2,sizeof(float));
    if (!buf) { aacDecoder_Close(aac); free(sample_offsets); if(st.sample_sizes)free(st.sample_sizes); if(st.chunk_offsets)free(st.chunk_offsets); if(st.stsc_entries)free(st.stsc_entries); free(raw); return false; }
    int total_frames=0, out_sample_rate=SAMPLE_RATE, out_channels=2;
    INT_PCM pcm_buf[2048*2];

    for (int i=0; i<st.num_samples && total_frames<MAX_FRAMES; i++) {
        uint64_t off=sample_offsets[i]; uint32_t sz=st.sample_sizes[i];
        if (off+sz>(uint64_t)file_size) continue;
        UCHAR *in_buf=raw+off; UINT in_size=sz, bytes_valid=in_size;
        if (aacDecoder_Fill(aac,&in_buf,&in_size,&bytes_valid)!=AAC_DEC_OK) continue;
        if (aacDecoder_DecodeFrame(aac,pcm_buf,sizeof(pcm_buf)/sizeof(INT_PCM),0)!=AAC_DEC_OK) continue;
        CStreamInfo *info=aacDecoder_GetStreamInfo(aac);
        if (!info||info->numChannels<1) continue;
        if (i==0) { out_sample_rate=info->sampleRate; out_channels=info->numChannels; }
        int frame_samples=info->frameSize, to_copy=std::min(frame_samples,MAX_FRAMES-total_frames);
        for (int j=0;j<to_copy;j++) {
            float L=pcm_buf[j*out_channels]/32768.0f;
            float R=(out_channels>1)?pcm_buf[j*out_channels+1]/32768.0f:L;
            buf[(total_frames+j)*2+0]=L; buf[(total_frames+j)*2+1]=R;
        }
        total_frames+=to_copy;
    }
    aacDecoder_Close(aac); free(sample_offsets);
    if(st.sample_sizes)free(st.sample_sizes); if(st.chunk_offsets)free(st.chunk_offsets); if(st.stsc_entries)free(st.stsc_entries);
    free(raw);

    if (total_frames==0) { free(buf); return false; }
    float *trimmed=(float*)realloc(buf,total_frames*2*sizeof(float));
    buf=trimmed?trimmed:buf;
    float duration_secs=(float)total_frames/(float)out_sample_rate;
    buf=resample_to_target(buf,total_frames,out_sample_rate,&total_frames);
    trk->audio_data=buf; trk->audio_frames=total_frames;
    trk->duration_secs=duration_secs;
    snprintf(trk->file_name,sizeof(trk->file_name),"%s",basename_ptr(path));
    snprintf(trk->file_path,sizeof(trk->file_path),"%s",path);
    host_log("[dj] loaded m4a: %s (%d frames, %.1fs, src %dHz)", trk->file_name, total_frames, trk->duration_secs, out_sample_rate);
    return true;
}

#endif

/* ------------------------------------------------------------------ */
/*  FLAC loader                                                       */
/* ------------------------------------------------------------------ */

#ifdef HAS_FLAC

static bool load_flac(track_t *trk, const char *path) {
    if (trk->audio_data) { free(trk->audio_data); trk->audio_data=nullptr; trk->audio_frames=0; trk->duration_secs=0; }
    FILE *fp=fopen(path,"rb"); if(!fp) return false;
    fseek(fp,0,SEEK_END); long file_size=ftell(fp); fseek(fp,0,SEEK_SET);
    if (file_size<8||file_size>500*1024*1024) { fclose(fp); return false; }
    uint8_t *raw=(uint8_t*)malloc(file_size);
    if (!raw) { fclose(fp); return false; }
    if ((long)fread(raw,1,file_size,fp)!=file_size) { free(raw); fclose(fp); return false; }
    fclose(fp);
    unsigned int channels=0, sample_rate=0; drflac_uint64 total_pcm;
    drflac_int16 *samples=drflac_open_memory_and_read_pcm_frames_s16(raw,(size_t)file_size,&channels,&sample_rate,&total_pcm,NULL);
    free(raw);
    if (!samples||total_pcm==0) { if(samples) drflac_free(samples,NULL); return false; }
    int total_frames=(int)total_pcm; if(total_frames>MAX_FRAMES) total_frames=MAX_FRAMES;
    float *buf=(float*)calloc(total_frames*2,sizeof(float));
    if (!buf) { drflac_free(samples,NULL); return false; }
    for (int i=0;i<total_frames;i++) {
        float L=samples[i*channels]/32768.0f;
        float R=(channels>1)?samples[i*channels+1]/32768.0f:L;
        buf[i*2+0]=L; buf[i*2+1]=R;
    }
    drflac_free(samples,NULL);
    float duration_secs=(float)total_frames/(float)sample_rate;
    buf=resample_to_target(buf,total_frames,sample_rate,&total_frames);
    trk->audio_data=buf; trk->audio_frames=total_frames;
    trk->duration_secs=duration_secs;
    snprintf(trk->file_name,sizeof(trk->file_name),"%s",basename_ptr(path));
    snprintf(trk->file_path,sizeof(trk->file_path),"%s",path);
    host_log("[dj] loaded flac: %s (%d frames, %.1fs, src %dHz)", trk->file_name, total_frames, trk->duration_secs, sample_rate);
    return true;
}

#endif

/* ------------------------------------------------------------------ */
/*  Auto-detect file format                                           */
/* ------------------------------------------------------------------ */

static bool has_extension(const char *path, const char *ext) {
    size_t plen=strlen(path), elen=strlen(ext);
    if (plen<elen) return false;
    const char *tail=path+plen-elen;
    for (size_t i=0;i<elen;i++) {
        char a=tail[i], b=ext[i];
        if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
        if (a!=b) return false;
    }
    return true;
}

static bool is_mod_file(const char *p) { return has_extension(p,".mod")||has_extension(p,".xm")||has_extension(p,".it")||has_extension(p,".s3m"); }
static bool is_mp3_file(const char *p) { return has_extension(p,".mp3"); }
static bool is_m4a_file(const char *p) { return has_extension(p,".m4a")||has_extension(p,".aac"); }
static bool is_flac_file(const char *p) { return has_extension(p,".flac"); }

static bool load_audio_file(track_t *trk, const char *path) {
    if (is_mp3_file(path)) {
#ifdef HAS_MP3
        return load_mp3(trk, path);
#else
        return false;
#endif
    }
    if (is_m4a_file(path)) {
#ifdef HAS_M4A
        return load_m4a(trk, path);
#else
        return false;
#endif
    }
    if (is_flac_file(path)) {
#ifdef HAS_FLAC
        return load_flac(trk, path);
#else
        return false;
#endif
    }
    return load_wav(trk, path);
}

/* ------------------------------------------------------------------ */
/*  BPM detection                                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  BPM Detection & Beat Grid — SOTA multi-band spectral flux +       */
/*  harmonic ACF + pulse-train refinement + downbeat detection         */
/* ------------------------------------------------------------------ */

struct beatgrid_result_t {
    float bpm;              /* exact BPM to 0.01 precision */
    double downbeat_frame;  /* frame position of first downbeat (-1 = not found) */
};

/* Number of frequency bands for multi-band onset detection */
static const int NUM_ODF_BANDS = 4;
/* Band edges in FFT bins (at 1024-pt FFT, 44100 Hz → ~43 Hz/bin)
 * Band 0: 0-200 Hz (bins 0-4)    — kick drums
 * Band 1: 200-800 Hz (bins 5-18)  — bass/low-mid
 * Band 2: 800-3000 Hz (bins 19-69) — mids/snares
 * Band 3: 3000-22050 Hz (bins 70-512) — hi-hats/cymbals */
static const int BAND_EDGES[NUM_ODF_BANDS + 1] = { 0, 5, 19, 70, 513 };
/* Weights: emphasize kick (band 0) and snare (band 2) for rhythm */
static const float BAND_WEIGHTS[NUM_ODF_BANDS] = { 2.0f, 0.8f, 1.5f, 0.5f };

static beatgrid_result_t detect_bpm_and_beatgrid(track_t *tracks, int max_frames) {
    beatgrid_result_t result = { 0.0f, -1.0 };
    if (max_frames < SAMPLE_RATE * 4) return result;

    /* Analyse full track (not just 30s) for maximum precision */
    int analyse_frames = max_frames;

    const int FFT_SIZE = 1024;
    const int HOP_SIZE = 512;
    int num_hops = (analyse_frames - FFT_SIZE) / HOP_SIZE;
    if (num_hops < 32) return result;

    /* Mix all stems to mono */
    float *mono = (float *)calloc(analyse_frames, sizeof(float));
    if (!mono) return result;
    for (int i = 0; i < analyse_frames; i++) {
        float s = 0;
        for (int t = 0; t < NUM_STEMS; t++) {
            if (!tracks[t].audio_data || i >= tracks[t].audio_frames) continue;
            s += tracks[t].audio_data[i*2+0] + tracks[t].audio_data[i*2+1];
        }
        mono[i] = s * 0.5f;
    }

    /* Setup pffft */
    PFFFT_Setup *fft = pffft_new_setup(FFT_SIZE, PFFFT_REAL);
    if (!fft) { free(mono); return result; }

    float *fft_in  = (float *)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float *fft_out = (float *)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float *fft_work = (float *)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    int num_bins = FFT_SIZE / 2 + 1;

    /* Per-band previous magnitude for spectral flux */
    float prev_band_mag[NUM_ODF_BANDS][513];
    memset(prev_band_mag, 0, sizeof(prev_band_mag));

    /* Onset strength signal (combined multi-band) */
    float *oss = (float *)calloc(num_hops, sizeof(float));
    /* Per-band onset strength (for downbeat detection later) */
    float *oss_bass = (float *)calloc(num_hops, sizeof(float)); /* band 0 only */

    /* Hann window */
    float window[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));

    /* Compute multi-band log-spectral-flux onset strength */
    for (int h = 0; h < num_hops; h++) {
        int offset = h * HOP_SIZE;

        for (int i = 0; i < FFT_SIZE; i++)
            fft_in[i] = mono[offset + i] * window[i];

        pffft_transform_ordered(fft, fft_in, fft_out, fft_work, PFFFT_FORWARD);

        /* Compute magnitude spectrum */
        float curr_mag[513];
        curr_mag[0] = fabsf(fft_out[0]);
        curr_mag[FFT_SIZE/2] = fabsf(fft_out[1]);
        for (int k = 1; k < FFT_SIZE/2; k++) {
            float re = fft_out[2*k];
            float im = fft_out[2*k+1];
            curr_mag[k] = sqrtf(re*re + im*im);
        }

        /* Multi-band log-spectral-flux */
        float combined_flux = 0;
        for (int b = 0; b < NUM_ODF_BANDS; b++) {
            float band_flux = 0;
            int bstart = BAND_EDGES[b];
            int bend = BAND_EDGES[b + 1];
            if (bend > num_bins) bend = num_bins;
            for (int k = bstart; k < bend; k++) {
                /* Log-magnitude domain — more perceptually relevant */
                float curr_log = logf(1.0f + curr_mag[k]);
                float prev_log = logf(1.0f + prev_band_mag[b][k]);
                float diff = curr_log - prev_log;
                if (diff > 0) band_flux += diff;
                prev_band_mag[b][k] = curr_mag[k];
            }
            combined_flux += band_flux * BAND_WEIGHTS[b];
            if (b == 0) oss_bass[h] = band_flux;
        }
        oss[h] = combined_flux;
    }

    free(mono);
    pffft_aligned_free(fft_in);
    pffft_aligned_free(fft_out);
    pffft_aligned_free(fft_work);
    pffft_destroy_setup(fft);

    /* ---- Stage 1: Coarse BPM via ACF + harmonic enhancement ---- */
    float hps = (float)SAMPLE_RATE / (float)HOP_SIZE; /* hops per second */
    int min_lag = (int)(hps * 60.0f / 200.0f);  /* 200 BPM */
    int max_lag = (int)(hps * 60.0f / 55.0f);   /* 55 BPM */
    if (max_lag >= num_hops / 2) max_lag = num_hops / 2 - 1;
    if (min_lag < 1) min_lag = 1;

    float *acf = (float *)calloc(max_lag + 1, sizeof(float));
    if (!acf) { free(oss); free(oss_bass); return result; }

    for (int lag = min_lag; lag <= max_lag; lag++) {
        float corr = 0;
        int count = num_hops - lag;
        for (int w = 0; w < count; w++)
            corr += oss[w] * oss[w + lag];
        acf[lag] = corr / (float)count;
    }

    /* Harmonic enhancement: reinforce lags whose 2x, 3x, 4x harmonics correlate */
    float best_score = 0;
    int best_lag = 0;
    for (int lag = min_lag; lag <= max_lag; lag++) {
        float score = acf[lag];
        if (lag * 2 <= max_lag)
            score *= (1.0f + acf[lag * 2]);
        if (lag * 3 <= max_lag)
            score *= (1.0f + acf[lag * 3] * 0.5f);
        if (lag * 4 <= max_lag)
            score *= (1.0f + acf[lag * 4] * 0.25f);
        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    /* Parabolic interpolation for sub-bin precision */
    float coarse_lag = (float)best_lag;
    if (best_lag > min_lag && best_lag < max_lag) {
        float a = acf[best_lag - 1];
        float b = acf[best_lag];
        float c = acf[best_lag + 1];
        float denom = 2.0f * (2.0f * b - a - c);
        if (fabsf(denom) > 1e-12f)
            coarse_lag = (float)best_lag + (a - c) / denom;
    }
    free(acf);

    if (coarse_lag <= 0) { free(oss); free(oss_bass); return result; }
    float coarse_bpm = 60.0f * hps / coarse_lag;

    /* Constrain to DJ range */
    while (coarse_bpm > 170.0f) coarse_bpm *= 0.5f;
    while (coarse_bpm < 70.0f)  coarse_bpm *= 2.0f;

    host_log("[dj] Coarse BPM: %.2f (multi-band ACF)", coarse_bpm);

    /* ---- Stage 2: Pulse-train cross-correlation refinement ---- */
    /* Sweep fine BPM candidates in 0.01 increments around coarse estimate.
     * For each candidate, generate an ideal pulse train and cross-correlate
     * with the OSS at all phase offsets. Best score wins. */
    float best_bpm = coarse_bpm;
    float best_pulse_score = -1e30f;
    int best_phase_hop = 0;

    float search_lo = coarse_bpm - 2.0f;
    float search_hi = coarse_bpm + 2.0f;
    if (search_lo < 55.0f) search_lo = 55.0f;
    if (search_hi > 200.0f) search_hi = 200.0f;

    for (float cand_bpm = search_lo; cand_bpm <= search_hi; cand_bpm += 0.01f) {
        float period_hops = hps * 60.0f / cand_bpm; /* beat period in hops */
        if (period_hops < 2.0f) continue;

        /* Try all phase offsets within one beat period */
        int max_phase = (int)period_hops;
        if (max_phase < 1) max_phase = 1;

        for (int phase = 0; phase < max_phase; phase++) {
            float score = 0;
            int pulse_count = 0;
            /* Walk through OSS at beat positions and sum values */
            float pos = (float)phase;
            while ((int)pos < num_hops) {
                int idx = (int)(pos + 0.5f);
                if (idx >= 0 && idx < num_hops) {
                    score += oss[idx];
                    pulse_count++;
                }
                pos += period_hops;
            }
            if (pulse_count > 0) score /= (float)pulse_count;

            if (score > best_pulse_score) {
                best_pulse_score = score;
                best_bpm = cand_bpm;
                best_phase_hop = phase;
            }
        }
    }

    /* ---- Stage 3: Ultra-fine refinement (0.001 BPM steps) ---- */
    float fine_lo = best_bpm - 0.05f;
    float fine_hi = best_bpm + 0.05f;
    for (float cand_bpm = fine_lo; cand_bpm <= fine_hi; cand_bpm += 0.001f) {
        float period_hops = hps * 60.0f / cand_bpm;
        if (period_hops < 2.0f) continue;
        int max_phase = (int)period_hops;
        if (max_phase < 1) max_phase = 1;
        for (int phase = 0; phase < max_phase; phase++) {
            float score = 0;
            int pulse_count = 0;
            float pos = (float)phase;
            while ((int)pos < num_hops) {
                int idx = (int)(pos + 0.5f);
                if (idx >= 0 && idx < num_hops) {
                    score += oss[idx];
                    pulse_count++;
                }
                pos += period_hops;
            }
            if (pulse_count > 0) score /= (float)pulse_count;
            if (score > best_pulse_score) {
                best_pulse_score = score;
                best_bpm = cand_bpm;
                best_phase_hop = phase;
            }
        }
    }

    /* Round to 0.01 */
    best_bpm = roundf(best_bpm * 100.0f) / 100.0f;
    result.bpm = best_bpm;
    host_log("[dj] Refined BPM: %.2f (pulse-train xcorr)", best_bpm);

    /* ---- Stage 4: Find beat positions for downbeat detection ---- */
    /* Using the refined BPM + best phase, place beat markers at OSS peaks
     * near expected positions, then find downbeat via bass energy pattern. */
    float period_hops = hps * 60.0f / best_bpm;
    int num_beats = 0;
    int max_beats = num_hops; /* upper bound */
    int *beat_hops = (int *)calloc(max_beats, sizeof(int));
    if (!beat_hops) { free(oss); free(oss_bass); return result; }

    /* Place beats: for each expected position, find local maximum within +/-3 hops */
    {
        float pos = (float)best_phase_hop;
        while ((int)(pos + 0.5f) < num_hops && num_beats < max_beats) {
            int expected = (int)(pos + 0.5f);
            /* Search window: 3 hops either side for local peak */
            int search_lo_h = std::max(0, expected - 3);
            int search_hi_h = std::min(num_hops - 1, expected + 3);
            int peak_idx = expected;
            float peak_val = -1e30f;
            for (int s = search_lo_h; s <= search_hi_h; s++) {
                if (oss[s] > peak_val) {
                    peak_val = oss[s];
                    peak_idx = s;
                }
            }
            beat_hops[num_beats++] = peak_idx;
            pos += period_hops;
        }
    }

    /* ---- Stage 5: Downbeat detection ---- */
    /* For each beat, compute bass energy. In 4/4 music, beat 1 (downbeat)
     * typically has the strongest kick. Score 4 phase hypotheses. */
    if (num_beats >= 8) {
        float phase_score[4] = {0, 0, 0, 0};
        for (int p = 0; p < 4; p++) {
            float downbeat_bass = 0;
            float other_bass = 0;
            int db_count = 0, oth_count = 0;
            for (int b = 0; b < num_beats; b++) {
                int bar_pos = (b - p + 400) % 4; /* which beat in the bar */
                float bass = oss_bass[beat_hops[b]];
                if (bar_pos == 0) {
                    downbeat_bass += bass;
                    db_count++;
                } else {
                    other_bass += bass;
                    oth_count++;
                }
            }
            if (db_count > 0) downbeat_bass /= (float)db_count;
            if (oth_count > 0) other_bass /= (float)oth_count;
            /* Score: how much stronger are "downbeats" than other beats */
            phase_score[p] = downbeat_bass - other_bass * 0.7f;
        }

        int best_phase = 0;
        for (int p = 1; p < 4; p++) {
            if (phase_score[p] > phase_score[best_phase])
                best_phase = p;
        }

        /* First downbeat = beat at index best_phase (or earliest beat
         * whose bar_pos == 0 under this phase hypothesis) */
        for (int b = 0; b < num_beats; b++) {
            if ((b % 4) == best_phase) {
                /* Convert hop index to frame position */
                result.downbeat_frame = (double)beat_hops[b] * (double)HOP_SIZE;
                host_log("[dj] Downbeat at frame %.0f (hop %d, phase %d)",
                         result.downbeat_frame, beat_hops[b], best_phase);
                break;
            }
        }
    }

    free(beat_hops);
    free(oss);
    free(oss_bass);

    host_log("[dj] Beat grid: BPM=%.2f downbeat=%.0f", result.bpm, result.downbeat_frame);
    return result;
}

/* Legacy wrapper */
static float detect_bpm_from_tracks(track_t *tracks, int max_frames) {
    beatgrid_result_t r = detect_bpm_and_beatgrid(tracks, max_frames);
    return r.bpm;
}

/* ---- Beat-grid-aware loop snapping ---- */

/* Snap a frame position to the nearest beat grid line.
 * Returns the snapped position, or the original if no beatgrid is set. */
static double snap_to_beat(deck_t *dk, double pos) {
    if (dk->beatgrid_bpm <= 0 || dk->beatgrid_downbeat < 0) return pos;
    double beat_frames = 60.0 / (double)dk->beatgrid_bpm * (double)SAMPLE_RATE;
    if (beat_frames < 1.0) return pos;
    /* Distance from downbeat in beats */
    double offset = pos - dk->beatgrid_downbeat;
    double beat_num = round(offset / beat_frames);
    return dk->beatgrid_downbeat + beat_num * beat_frames;
}

/* Compute exact loop size in frames from beat grid (not BPM approximation).
 * Uses beatgrid_bpm for precision when available. */
static int beats_to_frames_exact(float beats, deck_t *dk) {
    float bpm = dk->beatgrid_bpm > 0 ? dk->beatgrid_bpm : dk->bpm;
    if (bpm <= 0) bpm = 120.0f;
    return (int)(beats * 60.0f / bpm * (float)SAMPLE_RATE);
}

/* Compute 128-column waveform overview (peak values 0-15) */
static void compute_waveform(uint8_t *out, track_t *tracks, int max_frames) {
    memset(out, 0, 128);
    if (max_frames <= 0) return;

    float raw[128];
    int frames_per_col = max_frames / 128;
    if (frames_per_col < 1) frames_per_col = 1;
    /* Sample stride: skip frames for speed on long files */
    int stride = std::max(1, frames_per_col / 256);
    float global_max = 0;

    for (int col = 0; col < 128; col++) {
        int start = col * frames_per_col;
        int end = std::min(start + frames_per_col, max_frames);
        float peak = 0;
        for (int i = start; i < end; i += stride) {
            float mono = 0;
            for (int t = 0; t < NUM_STEMS; t++) {
                if (!tracks[t].audio_data || i >= tracks[t].audio_frames) continue;
                mono += fabsf(tracks[t].audio_data[i*2+0]) + fabsf(tracks[t].audio_data[i*2+1]);
            }
            if (mono > peak) peak = mono;
        }
        raw[col] = peak;
        if (peak > global_max) global_max = peak;
    }

    if (global_max > 0) {
        for (int col = 0; col < 128; col++)
            out[col] = (uint8_t)(raw[col] / global_max * 15.0f);
    }
}

/* ------------------------------------------------------------------ */
/*  Per-song cue persistence                                          */
/* ------------------------------------------------------------------ */

static const char *CUE_DIR = "/data/UserData/move-anything/dj_cues";

static void cue_file_path(char *out, size_t out_len, const char *song_path) {
    unsigned long hash = 5381;
    for (const char *p = song_path; *p; p++) hash = ((hash<<5)+hash) + (unsigned char)*p;
    snprintf(out, out_len, "%s/%08lx.cue", CUE_DIR, hash);
}

static const char *deck_song_key(deck_t *dk) {
    if (dk->loaded_file[0]) return dk->loaded_file;
    for (int t = 0; t < NUM_STEMS; t++)
        if (dk->tracks[t].file_path[0]) return dk->tracks[t].file_path;
    return nullptr;
}

static void ensure_cue_dir() {
    if (s_cue_dir_created) return;
    mkdir(CUE_DIR, 0755); /* no-op if exists, no fork */
    s_cue_dir_created = true;
}

static void save_cues(deck_t *dk) {
    const char *key = deck_song_key(dk);
    if (!key) return;
    ensure_cue_dir();
    char cpath[600]; cue_file_path(cpath, sizeof(cpath), key);
    FILE *f = fopen(cpath, "w"); if (!f) return;
    for (int c = 0; c < NUM_CUES; c++) fprintf(f, "%.0f\n", dk->cue_frames[c]);
    /* Line 9: beatgrid downbeat frame, Line 10: beatgrid BPM */
    fprintf(f, "%.0f\n", dk->beatgrid_downbeat);
    fprintf(f, "%.2f\n", dk->beatgrid_bpm);
    fclose(f);
}

/* Load cues + beatgrid from .cue file (backward-compatible: old files have 8 lines) */
static void load_cues(deck_t *dk) {
    const char *key = deck_song_key(dk);
    if (!key) return;
    char cpath[600]; cue_file_path(cpath, sizeof(cpath), key);
    FILE *f = fopen(cpath, "r"); if (!f) return;
    for (int c = 0; c < NUM_CUES; c++) {
        char line[64]; if (!fgets(line, sizeof(line), f)) break;
        dk->cue_frames[c] = atof(line);
    }
    /* Lines 9-10: beatgrid (optional, backward-compatible) */
    char line[64];
    if (fgets(line, sizeof(line), f)) {
        dk->beatgrid_downbeat = atof(line);
        if (fgets(line, sizeof(line), f)) {
            dk->beatgrid_bpm = (float)atof(line);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/*  Load file auto-detect                                             */
/* ------------------------------------------------------------------ */

/* Background loader thread function — uses snapshot from load_request_t,
 * NEVER reads dk->tracks[] (owned by render thread). */
static void *bg_load_thread(void *arg) {
    load_request_t *req = (load_request_t *)arg;
    deck_t *dk = req->dk;
    const char *path = req->path;
    int stem_slot = req->stem_slot;

    /* Initialize staging area from snapshot (no data race) */
    for (int t = 0; t < NUM_STEMS; t++) {
        memset(&dk->new_tracks[t], 0, sizeof(track_t));
        dk->new_tracks[t].volume = req->stem_volumes[t];
        dk->new_tracks[t].muted = 0;
    }

    bool ok = false;

    if (is_mod_file(path)) {
#ifdef HAS_LIBXMP
        deck_t *tmp = (deck_t *)calloc(1, sizeof(deck_t));
        if (tmp) {
            for (int t = 0; t < NUM_STEMS; t++) tmp->tracks[t].volume = 1.0f;
            ok = load_mod(tmp, path);
            if (ok) {
                memcpy(dk->new_tracks, tmp->tracks, sizeof(dk->new_tracks));
                for (int t = 0; t < NUM_STEMS; t++) tmp->tracks[t].audio_data = nullptr;
                dk->new_is_mod = 1;
                dk->new_max_frames = tmp->max_frames;
                dk->new_detected_bpm = tmp->bpm > 0 ? tmp->bpm : 120.0f;
                compute_waveform(dk->new_waveform, dk->new_tracks, dk->new_max_frames);
                for (int c = 0; c < NUM_CUES; c++) dk->new_cue_frames[c] = -1.0;
                dk->new_beatgrid_downbeat = -1.0;
                dk->new_beatgrid_bpm = 0.0f;
                char cpath[600]; cue_file_path(cpath, sizeof(cpath), path);
                FILE *f = fopen(cpath, "r");
                if (f) {
                    for (int c = 0; c < NUM_CUES; c++) {
                        char line[64];
                        if (!fgets(line, sizeof(line), f)) break;
                        dk->new_cue_frames[c] = atof(line);
                    }
                    char line[64];
                    if (fgets(line, sizeof(line), f)) {
                        dk->new_beatgrid_downbeat = atof(line);
                        if (fgets(line, sizeof(line), f))
                            dk->new_beatgrid_bpm = (float)atof(line);
                    }
                    fclose(f);
                }
            }
            free(tmp);
        }
#endif
    } else {
        if (stem_slot < 0 || stem_slot >= NUM_STEMS) stem_slot = 0;

        /* Restore non-target stems from snapshot (no touching dk->tracks) */
        for (int t = 0; t < NUM_STEMS; t++) {
            dk->new_tracks[t].audio_data = nullptr;
            dk->new_tracks[t].audio_frames = 0;
            dk->new_tracks[t].duration_secs = 0;
            dk->new_tracks[t].volume = req->stem_volumes[t];
            dk->new_tracks[t].muted = req->stem_muted[t];
            memcpy(dk->new_tracks[t].file_name, req->stem_file_names[t], sizeof(dk->new_tracks[t].file_name));
            memcpy(dk->new_tracks[t].file_path, req->stem_file_paths[t], sizeof(dk->new_tracks[t].file_path));
        }

        /* Use pre-copied audio data for non-target stems */
        if (!req->was_mod) {
            for (int t = 0; t < NUM_STEMS; t++) {
                if (t == stem_slot) continue;
                if (req->stem_audio_copies[t]) {
                    dk->new_tracks[t].audio_data = req->stem_audio_copies[t];
                    dk->new_tracks[t].audio_frames = req->stem_audio_frames[t];
                    dk->new_tracks[t].duration_secs = (float)req->stem_audio_frames[t] / (float)SAMPLE_RATE;
                    req->stem_audio_copies[t] = nullptr; /* ownership transferred */
                }
            }
        }

        dk->new_is_mod = 0;
        ok = load_audio_file(&dk->new_tracks[stem_slot], path);
        if (ok) {
            dk->new_max_frames = 0;
            for (int t = 0; t < NUM_STEMS; t++)
                if (dk->new_tracks[t].audio_frames > dk->new_max_frames)
                    dk->new_max_frames = dk->new_tracks[t].audio_frames;
            /* Detect BPM + beat grid (full-track analysis) */
            beatgrid_result_t bgr = detect_bpm_and_beatgrid(dk->new_tracks, dk->new_max_frames);
            dk->new_detected_bpm = bgr.bpm;
            dk->new_beatgrid_bpm = bgr.bpm;
            dk->new_beatgrid_downbeat = bgr.downbeat_frame;
            compute_waveform(dk->new_waveform, dk->new_tracks, dk->new_max_frames);
            for (int c = 0; c < NUM_CUES; c++) dk->new_cue_frames[c] = -1.0;
            char cpath[600]; cue_file_path(cpath, sizeof(cpath), path);
            FILE *f = fopen(cpath, "r");
            if (f) {
                for (int c = 0; c < NUM_CUES; c++) {
                    char line[64];
                    if (!fgets(line, sizeof(line), f)) break;
                    dk->new_cue_frames[c] = atof(line);
                }
                char line[64];
                if (fgets(line, sizeof(line), f)) {
                    dk->new_beatgrid_downbeat = atof(line);
                    if (fgets(line, sizeof(line), f))
                        dk->new_beatgrid_bpm = (float)atof(line);
                }
                fclose(f);
            }
        }
    }

    if (ok) {
        dk->load_state.store(2, std::memory_order_release); /* ready for swap */
    } else {
        for (int t = 0; t < NUM_STEMS; t++)
            if (dk->new_tracks[t].audio_data) { free(dk->new_tracks[t].audio_data); dk->new_tracks[t].audio_data = nullptr; }
        dk->load_state.store(0, std::memory_order_release);
    }

    /* Free any remaining snapshot copies that weren't transferred */
    for (int t = 0; t < NUM_STEMS; t++)
        if (req->stem_audio_copies[t]) free(req->stem_audio_copies[t]);
    free(req);
    return nullptr;
}

/* Check if background load is done and swap in new data.
 * Called from render_block — must not call free() or do I/O. */
static void check_load_complete(deck_t *dk) {
    if (dk->load_state.load(std::memory_order_acquire) != 2) return;

    /* Stop playback and swap track data */
    dk->playing = 0;
    dk->out_count = 0; dk->out_head = 0;

    /* Defer-free old audio data (never free on render thread) */
    for (int t = 0; t < NUM_STEMS; t++) {
        if (dk->tracks[t].audio_data) {
            deferred_free(dk->tracks[t].audio_data);
            dk->tracks[t].audio_data = nullptr;
        }
    }

    /* Move new data in */
    memcpy(dk->tracks, dk->new_tracks, sizeof(dk->tracks));
    for (int t = 0; t < NUM_STEMS; t++) dk->new_tracks[t].audio_data = nullptr;

    dk->is_mod = dk->new_is_mod;
    dk->max_frames = dk->new_max_frames;
    dk->detected_bpm = dk->new_detected_bpm;
    dk->beatgrid_downbeat = dk->new_beatgrid_downbeat;
    dk->beatgrid_bpm = dk->new_beatgrid_bpm;
    dk->bpm = dk->new_beatgrid_bpm > 0 ? dk->new_beatgrid_bpm
            : (dk->detected_bpm > 0 ? dk->detected_bpm : 120.0f);
    dk->vinyl_speed_pct = 100;
    memcpy(dk->loaded_file, dk->new_loaded_file, sizeof(dk->loaded_file));
    memcpy(dk->cue_frames, dk->new_cue_frames, sizeof(dk->cue_frames));
    memcpy(dk->waveform, dk->new_waveform, sizeof(dk->waveform));

    reset_stretcher(dk, 0.0);
    dk->load_state.store(0, std::memory_order_release);
}

static bool load_file_auto(deck_t *dk, const char *path, int stem_slot) {
    /* If already loading, ignore */
    int expected = 0;
    if (!dk->load_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        host_log("[dj] load ignored - already loading");
        return false;
    }

    /* Snapshot all stem state NOW on the calling thread (safe — we own the data).
     * The background thread will use this snapshot instead of reading dk->tracks. */
    load_request_t *req = (load_request_t *)calloc(1, sizeof(load_request_t));
    if (!req) { dk->load_state.store(0, std::memory_order_release); return false; }
    req->dk = dk;
    snprintf(req->path, sizeof(req->path), "%s", path);
    req->stem_slot = stem_slot;
    req->was_mod = dk->is_mod;
    for (int t = 0; t < NUM_STEMS; t++) {
        req->stem_volumes[t] = dk->tracks[t].volume;
        req->stem_muted[t] = dk->tracks[t].muted;
        memcpy(req->stem_file_names[t], dk->tracks[t].file_name, sizeof(req->stem_file_names[t]));
        memcpy(req->stem_file_paths[t], dk->tracks[t].file_path, sizeof(req->stem_file_paths[t]));
        req->stem_audio_frames[t] = dk->tracks[t].audio_frames;
        req->stem_audio_copies[t] = nullptr;
        /* Copy audio data for non-target stems (so bg thread has its own copy) */
        if (t != stem_slot && !dk->is_mod && dk->tracks[t].audio_data && dk->tracks[t].audio_frames > 0) {
            size_t sz = dk->tracks[t].audio_frames * 2 * sizeof(float);
            req->stem_audio_copies[t] = (float *)malloc(sz);
            if (req->stem_audio_copies[t]) {
                memcpy(req->stem_audio_copies[t], dk->tracks[t].audio_data, sz);
            }
        }
    }

    snprintf(dk->new_loaded_file, sizeof(dk->new_loaded_file), "%s", path);
    dk->new_stem_slot = stem_slot;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(&thread, &attr, bg_load_thread, req);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        host_log("[dj] failed to create load thread: %d", rc);
        for (int t = 0; t < NUM_STEMS; t++)
            if (req->stem_audio_copies[t]) free(req->stem_audio_copies[t]);
        free(req);
        dk->load_state.store(0, std::memory_order_release);
        return false;
    }
    host_log("[dj] background load started: %s (stem %d)", path, stem_slot);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Bungee helpers                                                    */
/* ------------------------------------------------------------------ */

/* Precomputed pitch ratios for -24..+24 semitones (avoids pow() in hot path) */
static const double PITCH_TABLE[49] = {
    /* -24 to -13: 2 octaves down to just above 1 octave down */
    0.25,         0.264865774, 0.280615512, 0.297301779, 0.314980262,
    0.333709964,  0.353553391, 0.374576769, 0.396850263, 0.420448208,
    0.445449359,  0.471937156,
    /* -12 to -1: 1 octave down to just below unison */
    0.5,          0.529731547, 0.561231024, 0.594603558, 0.629960525,
    0.667419927,  0.707106781, 0.749153538, 0.793700526, 0.840896415,
    0.890898718,  0.943874313,
    /* 0: unison */
    1.0,
    /* +1 to +12: just above unison to 1 octave up */
    1.059463094,  1.122462048, 1.189207115, 1.259921050, 1.334839854,
    1.414213562,  1.498307077, 1.587401052, 1.681792831, 1.781797436,
    1.887748625,  2.0,
    /* +13 to +24: just above 1 octave up to 2 octaves up */
    2.118926188,  2.244924097, 2.378414230, 2.519842100, 2.669679708,
    2.828427125,  2.996614154, 3.174802104, 3.363585661, 3.563594873,
    3.775497251,  4.0
};

static inline double pitch_multiplier(int semitones) {
    int idx = semitones + 24;
    if (idx < 0) idx = 0;
    if (idx > 48) idx = 48;
    return PITCH_TABLE[idx];
}

static void recalc_max_frames(deck_t *dk) {
    dk->max_frames = 0;
    for (int t = 0; t < NUM_STEMS; t++)
        if (dk->tracks[t].audio_frames > dk->max_frames)
            dk->max_frames = dk->tracks[t].audio_frames;
}

static void reset_stretcher(deck_t *dk, double position) {
    dk->req.position = position;
    dk->req.speed = (double)dk->speed;
    dk->req.pitch = pitch_multiplier(dk->pitch_semitones);
    dk->req.reset = true;
    dk->req.resampleMode = resampleMode_autoOut;
    dk->stretcher->preroll(dk->req);
}

static int beats_to_frames(float beats, float bpm) {
    if (bpm <= 0) bpm = 120.0f;
    return (int)(beats * 60.0f / bpm * (float)SAMPLE_RATE);
}

static void feed_grain_mixed(deck_t *dk, const Bungee::InputChunk &chunk) {
    const int len = chunk.end - chunk.begin;
    const int stride = dk->max_grain;
    float *__restrict__ grain_L = dk->grain_input;
    float *__restrict__ grain_R = dk->grain_input + stride;
    memset(grain_L, 0, stride * sizeof(float));
    memset(grain_R, 0, stride * sizeof(float));

    /* Hoist per-stem eligibility out of inner loop — the active stem set
     * changes rarely, but this loop runs thousands of times per block. */
    for (int t = 0; t < NUM_STEMS; t++) {
        const track_t *trk = &dk->tracks[t];
        if (trk->muted || !trk->audio_data || trk->audio_frames <= 0) continue;
        const float *__restrict__ audio = trk->audio_data;
        const float vol = trk->volume;
        const int frames = trk->audio_frames;

        /* Clamp iteration range to valid audio bounds */
        int i_start = 0, i_end = len;
        if (chunk.begin < 0) i_start = -chunk.begin;
        if (chunk.begin + len > frames) i_end = frames - chunk.begin;
        if (i_end <= i_start) continue;

        const int base = chunk.begin;
        for (int i = i_start; i < i_end; i++) {
            const int src2 = (base + i) * 2;
            grain_L[i] += audio[src2]   * vol;
            grain_R[i] += audio[src2+1] * vol;
        }
    }
    dk->stretcher->analyseGrain(dk->grain_input, stride,
        std::max(0, -chunk.begin), std::max(0, chunk.end - dk->max_frames));
}

/* ------------------------------------------------------------------ */
/*  Deck init / destroy                                               */
/* ------------------------------------------------------------------ */

static void init_deck(deck_t *dk) {
    memset(dk, 0, sizeof(deck_t));
    dk->speed_pct = 100; dk->speed = 1.0f;
    dk->bpm = 120.0f; dk->vinyl_speed_pct = 100;
    dk->filter_pos = 50;
    dk->stutter_filter_pos = 50;  /* bypass */
    dk->stutter_pitch_semis = 0.0f;
    dk->load_state.store(0);
    for (int t = 0; t < NUM_STEMS; t++) dk->tracks[t].volume = 1.0f;
    for (int c = 0; c < NUM_CUES; c++) dk->cue_frames[c] = -1.0;
    dk->beatgrid_downbeat = -1.0;
    dk->beatgrid_bpm = 0.0f;
    dk->loop_beats_idx = 6;
    dk->slip_mode = 0;
    dk->slip_position = 0;
    dk->slip_engaged = 0;

    dk->stretcher = new Bungee::Stretcher<Bungee::Basic>(
        Bungee::SampleRates{SAMPLE_RATE, SAMPLE_RATE}, 2, 0);
    dk->max_grain = dk->stretcher->maxInputFrameCount();
    dk->grain_input = (float *)calloc(dk->max_grain * 2, sizeof(float));
    dk->out_buf = (float *)calloc(OUT_BUF_CAPACITY * 2, sizeof(float));

    dk->req.position = 0; dk->req.speed = 1.0; dk->req.pitch = 1.0;
    dk->req.reset = true; dk->req.resampleMode = resampleMode_autoOut;
}

static void destroy_deck(deck_t *dk) {
    if (dk->stretcher) delete dk->stretcher;
    if (dk->grain_input) free(dk->grain_input);
    if (dk->out_buf) free(dk->out_buf);
    for (int t = 0; t < NUM_STEMS; t++)
        if (dk->tracks[t].audio_data) free(dk->tracks[t].audio_data);
}

/* ------------------------------------------------------------------ */
/*  Render a single deck to float buffer                              */
/* ------------------------------------------------------------------ */

static void render_deck(deck_t *dk, float *out_lr, int frames) {
    if (!dk->playing || dk->max_frames <= 0 || !dk->stretcher) {
        memset(out_lr, 0, frames * 2 * sizeof(float));
        return;
    }

    double vinyl = dk->vinyl_speed_pct / 100.0;
    dk->req.speed = (double)dk->speed * vinyl;
    dk->req.pitch = pitch_multiplier(dk->pitch_semitones) * vinyl;

    /* ---- Stutter: bypass Bungee, clean repeats with crossfades ---- */
    if (dk->stutter_active && dk->stutter_size_frames > 0) {
        double spos = dk->stutter_start;
        double sz = (double)dk->stutter_size_frames;

        /* Micro-crossfade length: ~3ms at 44100 Hz = 132 samples */
        const int XFADE_FRAMES = 132;
        double xfade_len = (double)std::min(XFADE_FRAMES, dk->stutter_size_frames / 4);

        /* Knob-controlled pitch shift (changes playback rate) */
        double pitch_ratio = pow(2.0, (double)dk->stutter_pitch_semis / 12.0);
        double base_rate = (double)dk->speed * vinyl * pitch_ratio;

        /* Pitch ramp: steady descent of ~0.4 semitones per repeat.
         * ~20% of repeats get a random musical jump that interrupts the ramp,
         * then the ramp continues from the new pitch — slip-pitch stutter. */
        static const float JUMP_INTERVALS[] = {
            12, 7, 5, 4, 3,    /* octave, fifth, fourth, maj3, min3 up */
            -12, -7, -5,       /* octave, fifth, fourth down */
            -3, 2, -2, 1,      /* min3 down, whole tone, semitone */
        };
        static const int NUM_JUMPS = sizeof(JUMP_INTERVALS) / sizeof(JUMP_INTERVALS[0]);
        const double RAMP_SEMIS_PER_REPEAT = -0.4;

        for (int i = 0; i < frames; i++) {
            /* Retrigger: clean wrap back to start */
            if (dk->stutter_pos >= sz || dk->stutter_pos < 0) {
                dk->stutter_pos = (dk->stutter_pos >= sz) ? dk->stutter_pos - sz : 0;
                dk->stutter_repeat++;

                /* xorshift PRNG */
                dk->stutter_rng ^= dk->stutter_rng << 13;
                dk->stutter_rng ^= dk->stutter_rng >> 7;
                dk->stutter_rng ^= dk->stutter_rng << 17;

                /* ~30% chance of reverse */
                dk->stutter_reversed = (dk->stutter_rng % 10) < 3 ? 1 : 0;

                /* Ramp the accumulated pitch down */
                dk->stutter_rand_pitch += (float)RAMP_SEMIS_PER_REPEAT;

                /* ~20% chance of a random musical jump on top of the ramp */
                if ((dk->stutter_rng % 5) == 0) {
                    dk->stutter_rng ^= dk->stutter_rng << 13;
                    dk->stutter_rng ^= dk->stutter_rng >> 7;
                    dk->stutter_rng ^= dk->stutter_rng << 17;
                    dk->stutter_rand_pitch += JUMP_INTERVALS[dk->stutter_rng % NUM_JUMPS];
                }
                /* Clamp so it doesn't go totally insane */
                if (dk->stutter_rand_pitch < -24.0f) dk->stutter_rand_pitch = -24.0f;
                if (dk->stutter_rand_pitch > 24.0f) dk->stutter_rand_pitch = 24.0f;

                /* Reverse starts at end of window */
                if (dk->stutter_reversed) dk->stutter_pos = sz - 1;
            }

            /* Accumulated pitch (ramp + jumps) applied to playback rate */
            double repeat_pitch = pow(2.0, (double)dk->stutter_rand_pitch / 12.0);
            double playback_rate = base_rate * repeat_pitch;
            /* Reverse direction */
            if (dk->stutter_reversed) playback_rate = -playback_rate;

            /* Raised-cosine crossfade at loop boundary for click-free retrigger:
             * near the end/start of the window, blend with the wrap point */
            double dist_to_edge;
            if (dk->stutter_reversed)
                dist_to_edge = dk->stutter_pos;  /* distance to start (reverse) */
            else
                dist_to_edge = sz - dk->stutter_pos;  /* distance to end (forward) */
            float xfade_mix = 1.0f;
            if (dist_to_edge < xfade_len && xfade_len > 0) {
                float t = (float)(dist_to_edge / xfade_len);
                xfade_mix = 0.5f * (1.0f + cosf((1.0f - t) * 3.14159265f));
            }

            int frame_cur = (int)(spos + dk->stutter_pos);
            if (frame_cur < 0) frame_cur = 0;
            if (frame_cur >= dk->max_frames) frame_cur = dk->max_frames - 1;

            /* Read audio at current position */
            float L = 0.0f, R = 0.0f;
            for (int t = 0; t < NUM_STEMS; t++) {
                const track_t *trk = &dk->tracks[t];
                if (trk->muted || !trk->audio_data || frame_cur >= trk->audio_frames)
                    continue;
                L += trk->audio_data[frame_cur * 2]     * trk->volume;
                R += trk->audio_data[frame_cur * 2 + 1] * trk->volume;
            }

            /* Crossfade: blend with audio from the wrap point */
            if (xfade_mix < 0.999f) {
                /* For forward: near end, blend with start. For reverse: near start, blend with end */
                double wrap_pos = dk->stutter_reversed
                    ? (sz - 1 - (dist_to_edge))   /* near start → blend with end */
                    : fmod(dk->stutter_pos + dist_to_edge, sz);  /* near end → blend with start */
                if (wrap_pos < 0) wrap_pos = 0;
                if (wrap_pos >= sz) wrap_pos = sz - 1;
                int frame_start = (int)(spos + wrap_pos);
                if (frame_start < 0) frame_start = 0;
                if (frame_start >= dk->max_frames) frame_start = dk->max_frames - 1;

                float sL = 0.0f, sR = 0.0f;
                for (int t = 0; t < NUM_STEMS; t++) {
                    const track_t *trk = &dk->tracks[t];
                    if (trk->muted || !trk->audio_data || frame_start >= trk->audio_frames)
                        continue;
                    sL += trk->audio_data[frame_start * 2]     * trk->volume;
                    sR += trk->audio_data[frame_start * 2 + 1] * trk->volume;
                }
                L = L * xfade_mix + sL * (1.0f - xfade_mix);
                R = R * xfade_mix + sR * (1.0f - xfade_mix);
            }

            /* Stutter filter: same LPF/HPF as DJ filter, knob-controlled */
            if (dk->stutter_filter_pos < 50) {
                float ft = dk->stutter_filter_pos / 50.0f;
                float alpha = 0.01f + ft * ft * 0.99f;
                dk->stutter_filter_z1 += alpha * (L - dk->stutter_filter_z1);
                dk->stutter_filter_z2 += alpha * (R - dk->stutter_filter_z2);
                L = dk->stutter_filter_z1;
                R = dk->stutter_filter_z2;
            } else if (dk->stutter_filter_pos > 50) {
                float ft = (dk->stutter_filter_pos - 50) / 50.0f;
                float alpha = 0.01f + ft * ft * 0.99f;
                dk->stutter_filter_z1 += alpha * (L - dk->stutter_filter_z1);
                dk->stutter_filter_z2 += alpha * (R - dk->stutter_filter_z2);
                L = L - dk->stutter_filter_z1;
                R = R - dk->stutter_filter_z2;
            }

            out_lr[i * 2]     = L;
            out_lr[i * 2 + 1] = R;

            dk->stutter_pos += playback_rate;
        }

        /* Keep Bungee's position in sync so playback resumes correctly
         * when stutter is released (non-slip mode) */
        dk->req.position = spos + dk->stutter_pos;

        /* Advance slip position */
        if (dk->slip_mode) {
            double effective_speed = (double)dk->speed * vinyl;
            dk->slip_position += effective_speed * frames;
            if (dk->slip_position >= dk->max_frames) dk->slip_position = dk->max_frames - 1;
            if (dk->slip_position < 0) dk->slip_position = 0;
        }

        /* Apply DJ filter */
        if (dk->filter_pos != 50) {
            if (dk->filter_pos < 50) {
                float t = dk->filter_pos / 50.0f;
                float alpha = 0.01f + t * t * 0.99f;
                for (int i = 0; i < frames; i++) {
                    dk->flt_lp_l += alpha * (out_lr[i*2+0] - dk->flt_lp_l);
                    dk->flt_lp_r += alpha * (out_lr[i*2+1] - dk->flt_lp_r);
                    out_lr[i*2+0] = dk->flt_lp_l;
                    out_lr[i*2+1] = dk->flt_lp_r;
                }
            } else {
                float t = (dk->filter_pos - 50) / 50.0f;
                float alpha = 0.01f + t * t * 0.99f;
                for (int i = 0; i < frames; i++) {
                    float inL = out_lr[i*2+0], inR = out_lr[i*2+1];
                    dk->flt_hp_l += alpha * (inL - dk->flt_hp_l);
                    dk->flt_hp_r += alpha * (inR - dk->flt_hp_r);
                    out_lr[i*2+0] = inL - dk->flt_hp_l;
                    out_lr[i*2+1] = inR - dk->flt_hp_r;
                }
            }
        }
        return;
    }
    /* ---- End stutter bypass ---- */

    int safety = frames * 4;
    while (dk->out_count < frames && safety-- > 0) {
        double pos = dk->req.position;

        if (dk->loop_active && dk->loop_size_frames > 0) {
            double lpos = dk->loop_start;
            double lsz = (double)dk->loop_size_frames;
            if (pos >= lpos + lsz) {
                /* Simple wrap — typically overshoots by <1 grain.
                 * Don't call preroll() — it subtracts an input hop which can
                 * push position before the loop start on very short loops. */
                double off = pos - lpos;
                while (off >= lsz) off -= lsz;
                dk->req.position = lpos + off;
                dk->req.reset = true;
            }
        }

        if (dk->req.position >= dk->max_frames) {
            dk->playing = false;
            dk->req.position = 0;
            break;
        }

        Bungee::InputChunk chunk = dk->stretcher->specifyGrain(dk->req);
        feed_grain_mixed(dk, chunk);
        Bungee::OutputChunk output{};
        dk->stretcher->synthesiseGrain(output);

        int n = output.frameCount;
        if (dk->out_count + n > OUT_BUF_CAPACITY) n = OUT_BUF_CAPACITY - dk->out_count;

        /* Ring buffer write: wrap around OUT_BUF_CAPACITY */
        int wr = (dk->out_head + dk->out_count) % OUT_BUF_CAPACITY;
        for (int i = 0; i < n; i++) {
            int idx = (wr + i) % OUT_BUF_CAPACITY;
            dk->out_buf[idx*2+0] = output.data[i];
            dk->out_buf[idx*2+1] = output.data[output.channelStride+i];
        }
        dk->out_count += n;
        dk->stretcher->next(dk->req);
        dk->req.reset = false;
    }

    /* Advance slip position (shadow playback that ignores loops/cues) */
    if (dk->slip_mode) {
        double effective_speed = (double)dk->speed * (dk->vinyl_speed_pct / 100.0);
        dk->slip_position += effective_speed * frames;
        if (dk->slip_position >= dk->max_frames) dk->slip_position = dk->max_frames - 1;
        if (dk->slip_position < 0) dk->slip_position = 0;
    }

    /* Ring buffer read: no memmove needed */
    int drain = std::min(dk->out_count, frames);
    for (int i = 0; i < drain; i++) {
        int idx = (dk->out_head + i) % OUT_BUF_CAPACITY;
        out_lr[i*2+0] = dk->out_buf[idx*2+0];
        out_lr[i*2+1] = dk->out_buf[idx*2+1];
    }
    if (drain < frames) memset(out_lr + drain*2, 0, (frames-drain)*2*sizeof(float));
    dk->out_head = (dk->out_head + drain) % OUT_BUF_CAPACITY;
    dk->out_count -= drain;

    /* Apply DJ filter (one-pole LPF or HPF) */
    if (dk->filter_pos != 50) {
        if (dk->filter_pos < 50) {
            /* LPF: 0=heavy, 49=light. Coefficient: 0.01 (heavy) to ~1.0 (bypass) */
            float t = dk->filter_pos / 50.0f;  /* 0..0.98 */
            float alpha = 0.01f + t * t * 0.99f;
            for (int i = 0; i < frames; i++) {
                dk->flt_lp_l += alpha * (out_lr[i*2+0] - dk->flt_lp_l);
                dk->flt_lp_r += alpha * (out_lr[i*2+1] - dk->flt_lp_r);
                out_lr[i*2+0] = dk->flt_lp_l;
                out_lr[i*2+1] = dk->flt_lp_r;
            }
        } else {
            /* HPF: 51=light, 100=heavy. Higher alpha = heavier HP filter */
            float t = (dk->filter_pos - 50) / 50.0f;  /* 0.02..1.0 */
            float alpha = 0.01f + t * t * 0.99f;
            for (int i = 0; i < frames; i++) {
                float inL = out_lr[i*2+0], inR = out_lr[i*2+1];
                dk->flt_hp_l += alpha * (inL - dk->flt_hp_l);
                dk->flt_hp_r += alpha * (inR - dk->flt_hp_r);
                out_lr[i*2+0] = inL - dk->flt_hp_l;
                out_lr[i*2+1] = inR - dk->flt_hp_r;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Plugin API v2 callbacks                                           */
/* ------------------------------------------------------------------ */

static void *dj_create(const char *module_dir, const char *json_defaults) {
    instance_t *inst = (instance_t *)calloc(1, sizeof(instance_t));
    if (!inst) return nullptr;
    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir);

    inst->master_vol = 1.0f;
    inst->crossfader = 50;
    inst->deck_vol[0] = 1.0f;
    inst->deck_vol[1] = 1.0f;

    init_deck(&inst->decks[0]);
    init_deck(&inst->decks[1]);

    /* Cache-line aligned render buffers — prevents false sharing between cores */
    posix_memalign((void **)&inst->render_buf_a, 64, RENDER_BUF_SIZE * sizeof(float));
    posix_memalign((void **)&inst->render_buf_b, 64, RENDER_BUF_SIZE * sizeof(float));
    memset(inst->render_buf_a, 0, RENDER_BUF_SIZE * sizeof(float));
    memset(inst->render_buf_b, 0, RENDER_BUF_SIZE * sizeof(float));

    /* Persistent worker thread for parallel deck B rendering */
    inst->worker = create_render_worker();

    host_log("[dj] dual-deck instance created (parallel render: %s)",
             inst->worker ? "yes" : "no, fallback sequential");
    return inst;
}

static void dj_destroy(void *ptr) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst) return;
    destroy_render_worker(inst->worker);
    destroy_deck(&inst->decks[0]);
    destroy_deck(&inst->decks[1]);
    if (inst->render_buf_a) free(inst->render_buf_a);
    if (inst->render_buf_b) free(inst->render_buf_b);
    free_playlists();
    free(inst);
}

static void dj_on_midi(void *ptr, const uint8_t *msg, int len, int source) {
    (void)ptr; (void)msg; (void)len; (void)source;
}

/* Per-deck param handler */
static void set_deck_param(deck_t *dk, const char *key, const char *val) {
    if (strcmp(key, "load_file") == 0 || strcmp(key, "file_path") == 0) {
        load_file_auto(dk, val, 0); return;
    }

    /* stem_path_N, stem_mute_N, stem_vol_N — direct index parse */
    if (strncmp(key, "stem_", 5) == 0) {
        const char *sub = key + 5;
        if (strncmp(sub, "path_", 5) == 0) { int t = sub[5] - '0'; if (t >= 0 && t < NUM_STEMS) load_file_auto(dk, val, t); return; }
        if (strncmp(sub, "mute_", 5) == 0) { int t = sub[5] - '0'; if (t >= 0 && t < NUM_STEMS) dk->tracks[t].muted = atoi(val) ? 1 : 0; return; }
        if (strncmp(sub, "vol_", 4) == 0)  { int t = sub[4] - '0'; if (t >= 0 && t < NUM_STEMS) { int v=atoi(val); dk->tracks[t].volume = std::max(0,std::min(100,v))/100.0f; } return; }
    }

    if (strcmp(key, "playing") == 0) {
        int v = atoi(val);
        if (v && !dk->playing && dk->stretcher && dk->max_frames > 0) {
            dk->out_count = 0; dk->out_head = 0; reset_stretcher(dk, dk->req.position);
        }
        dk->playing = v ? 1 : 0;
    }
    else if (strcmp(key, "pitch_semitones") == 0) {
        int v = atoi(val); dk->pitch_semitones = std::max(-24, std::min(24, v));
    }
    else if (strcmp(key, "speed_pct") == 0) {
        int v = std::max(50, std::min(200, atoi(val)));
        dk->speed_pct = v; dk->speed = v / 100.0f;
    }
    else if (strcmp(key, "vinyl_speed") == 0) {
        dk->vinyl_speed_pct = std::max(50, std::min(150, atoi(val)));
    }
    else if (strcmp(key, "seek") == 0) {
        double frac = std::max(0.0, std::min(1.0, atof(val)));
        dk->loop_active = 0;
        dk->stutter_active = 0;
        dk->out_count = 0; dk->out_head = 0; reset_stretcher(dk, frac * (double)dk->max_frames);
    }
    else if (strcmp(key, "set_cue") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < NUM_CUES) { dk->cue_frames[n] = dk->req.position; dk->cues_dirty = 1; }
    }
    else if (strcmp(key, "jump_cue") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < NUM_CUES && dk->cue_frames[n] >= 0.0) {
            /* Always clear loop/stutter on cue jump — prevents ghost loops
             * from race between UI and audio threads */
            dk->loop_active = 0;
            dk->stutter_active = 0;
            /* In slip mode: reset slip position to the cue point.
             * This means subsequent hot loops will snap back to the cue,
             * not the original pre-cue position. */
            if (dk->slip_mode) {
                dk->slip_position = dk->cue_frames[n];
                dk->slip_engaged = 0;
            }
            dk->out_count = 0; dk->out_head = 0; reset_stretcher(dk, dk->cue_frames[n]);
        }
    }
    else if (strcmp(key, "clear_cue") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < NUM_CUES) { dk->cue_frames[n] = -1.0; dk->cues_dirty = 1; }
    }
    else if (strcmp(key, "stutter_active") == 0) {
        int v = atoi(val);
        if (v && !dk->stutter_active) {
            /* Calculate the audible position (what the listener currently hears)
             * by subtracting the buffered-ahead audio from the stretcher position.
             * req.position is where the stretcher will READ NEXT, but the user
             * hears audio that was rendered out_count frames ago. */
            double audible_pos = dk->req.position;
            if (dk->out_count > 0 && dk->speed > 0) {
                audible_pos -= (double)dk->out_count * dk->speed * (dk->vinyl_speed_pct / 100.0);
                if (audible_pos < 0) audible_pos = 0;
            }
            dk->stutter_start = audible_pos;
            dk->stutter_pos = 0.0;  /* start at beginning of stutter region */
            host_log("[dj] STUTTER ACTIVATE: start=%.0f size=%d frames, audible=%.0f req=%.0f out_count=%d",
                audible_pos, dk->stutter_size_frames, audible_pos, dk->req.position, dk->out_count);
            dk->stutter_repeat = 0;
            dk->stutter_reversed = 0;
            dk->stutter_rand_pitch = 0.0f;
            dk->stutter_rng = (uint32_t)(audible_pos) ^ 0xDEADBEEF;
            if (dk->stutter_rng == 0) dk->stutter_rng = 1;
            dk->stutter_filter_z1 = 0.0f;
            dk->stutter_filter_z2 = 0.0f;
            if (dk->slip_mode) dk->slip_engaged = 1;
        }
        if (!v && dk->stutter_active) {
            dk->stutter_filter_z1 = 0.0f;
            dk->stutter_filter_z2 = 0.0f;
            dk->out_count = 0; dk->out_head = 0;
            if (dk->slip_mode && dk->slip_engaged) {
                /* Slip snap-back: return to shadow position */
                reset_stretcher(dk, dk->slip_position);
                dk->slip_engaged = 0;
            } else {
                /* No slip: just reset stretcher at current position to clear pitch */
                reset_stretcher(dk, dk->req.position);
            }
        }
        dk->stutter_active = v ? 1 : 0;
    }
    else if (strcmp(key, "stutter_size") == 0) {
        int idx = std::max(0, std::min(NUM_STUTTER_SIZES-1, atoi(val)));
        dk->stutter_size_frames = beats_to_frames_exact(STUTTER_BEATS[idx], dk);
        if (dk->stutter_size_frames < 64) dk->stutter_size_frames = 64;
    }
    else if (strcmp(key, "stutter_go") == 0) {
        /* Atomic set-size-and-activate: the framework coalesces back-to-back
         * sendParamNow calls so stutter_size + stutter_active as two params
         * doesn't work. This single param does both. */
        int idx = std::max(0, std::min(NUM_STUTTER_SIZES-1, atoi(val)));
        dk->stutter_size_frames = beats_to_frames_exact(STUTTER_BEATS[idx], dk);
        if (dk->stutter_size_frames < 64) dk->stutter_size_frames = 64;
        if (!dk->stutter_active) {
            double audible_pos = dk->req.position;
            if (dk->out_count > 0 && dk->speed > 0) {
                audible_pos -= (double)dk->out_count * dk->speed * (dk->vinyl_speed_pct / 100.0);
                if (audible_pos < 0) audible_pos = 0;
            }
            dk->stutter_start = audible_pos;
            dk->stutter_pos = 0.0;
            dk->stutter_repeat = 0;
            dk->stutter_reversed = 0;
            dk->stutter_rand_pitch = 0.0f;
            dk->stutter_rng = (uint32_t)(audible_pos) ^ 0xDEADBEEF;
            if (dk->stutter_rng == 0) dk->stutter_rng = 1;
            dk->stutter_filter_z1 = 0.0f;
            dk->stutter_filter_z2 = 0.0f;
            /* pitch and filter are preserved across activations so knob position persists */
            host_log("[dj] STUTTER GO: start=%.0f size=%d frames", audible_pos, dk->stutter_size_frames);
            if (dk->slip_mode) dk->slip_engaged = 1;
        }
        dk->stutter_active = 1;
    }
    else if (strcmp(key, "stutter_pitch") == 0) {
        dk->stutter_pitch_semis = std::max(-12.0f, std::min(12.0f, (float)atof(val)));
    }
    else if (strcmp(key, "stutter_filter") == 0) {
        dk->stutter_filter_pos = std::max(0, std::min(100, atoi(val)));
    }
    else if (strcmp(key, "beatgrid_downbeat") == 0) {
        dk->beatgrid_downbeat = atof(val);
        dk->cues_dirty = 1;
    }
    else if (strcmp(key, "beatgrid_bpm") == 0) {
        float v = (float)atof(val);
        dk->beatgrid_bpm = v;
        if (v > 0) dk->bpm = v;
        dk->cues_dirty = 1;
    }
    else if (strcmp(key, "loop_active") == 0) {
        int v = atoi(val);
        if (v) {
            dk->loop_start = snap_to_beat(dk, dk->req.position);
            dk->loop_size_frames = beats_to_frames_exact(LOOP_BEATS[dk->loop_beats_idx], dk);
            if (dk->loop_size_frames < 128) dk->loop_size_frames = 128;
            if (dk->slip_mode) dk->slip_engaged = 1;
        }
        if (!v && dk->loop_active && dk->slip_mode && dk->slip_engaged) {
            dk->out_count = 0; dk->out_head = 0;
            reset_stretcher(dk, dk->slip_position);
            dk->slip_engaged = 0;
        }
        dk->loop_active = v ? 1 : 0;
    }
    else if (strcmp(key, "loop_size") == 0) {
        int idx = std::max(0, std::min(NUM_LOOP_SIZES-1, atoi(val)));
        dk->loop_beats_idx = idx;
        dk->loop_size_frames = beats_to_frames_exact(LOOP_BEATS[idx], dk);
        if (dk->loop_size_frames < 128) dk->loop_size_frames = 128;
    }
    else if (strcmp(key, "hot_loop") == 0) {
        /* Atomic set-size-and-activate: val = loop index, or -1 to deactivate */
        int idx = atoi(val);
        if (idx < 0) {
            if (dk->loop_active && dk->slip_mode && dk->slip_engaged) {
                dk->out_count = 0; dk->out_head = 0;
                reset_stretcher(dk, dk->slip_position);
                dk->slip_engaged = 0;
            }
            dk->loop_active = 0;
        } else {
            idx = std::max(0, std::min(NUM_LOOP_SIZES-1, idx));
            dk->loop_beats_idx = idx;
            dk->loop_size_frames = beats_to_frames_exact(LOOP_BEATS[idx], dk);
            if (dk->loop_size_frames < 128) dk->loop_size_frames = 128;
            /* Snap loop start to nearest beat grid line for tight alignment */
            dk->loop_start = snap_to_beat(dk, dk->req.position);
            dk->loop_active = 1;
            if (dk->slip_mode) dk->slip_engaged = 1;
        }
    }
    else if (strcmp(key, "cue_set_pos") == 0) {
        /* Set cue at arbitrary position: "idx fraction" — no save, just update in memory */
        int idx; float frac;
        if (sscanf(val, "%d %f", &idx, &frac) == 2 && idx >= 0 && idx < NUM_CUES && dk->max_frames > 0) {
            frac = std::max(0.0f, std::min(1.0f, frac));
            dk->cue_frames[idx] = (double)frac * dk->max_frames;
        }
    }
    else if (strcmp(key, "save_cues") == 0) {
        save_cues(dk);
    }
    else if (strcmp(key, "slip_mode") == 0) {
        int v = atoi(val);
        if (v && !dk->slip_mode) {
            /* Turning on: capture current position as shadow */
            dk->slip_position = dk->req.position;
            dk->slip_engaged = 0;
        }
        dk->slip_mode = v ? 1 : 0;
    }
    else if (strcmp(key, "filter") == 0) {
        dk->filter_pos = std::max(0, std::min(100, atoi(val)));
    }
}

static void dj_set_param(void *ptr, const char *key, const char *val) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst || !key || !val) return;
    host_log("[dj] set_param: key='%s' val='%.60s'", key, val);

    /* Framework file_path goes to deck A */
    if (strcmp(key, "file_path") == 0) { load_file_auto(&inst->decks[0], val, 0); return; }

    /* Global mixer params */
    if (strcmp(key, "crossfader") == 0) { inst->crossfader = std::max(0, std::min(100, atoi(val))); return; }
    if (strcmp(key, "a_vol") == 0) { inst->deck_vol[0] = std::max(0, std::min(175, atoi(val))) / 100.0f; return; }
    if (strcmp(key, "b_vol") == 0) { inst->deck_vol[1] = std::max(0, std::min(175, atoi(val))) / 100.0f; return; }
    if (strcmp(key, "master_vol") == 0) { inst->master_vol = std::max(0, std::min(100, atoi(val))) / 100.0f; return; }

    /* Deck-prefixed params: a_xxx or b_xxx */
    if (key[0] == 'a' && key[1] == '_') { set_deck_param(&inst->decks[0], key+2, val); return; }
    if (key[0] == 'b' && key[1] == '_') { set_deck_param(&inst->decks[1], key+2, val); return; }
}

/* ------------------------------------------------------------------ */
/*  Playlist directory scanner                                        */
/* ------------------------------------------------------------------ */

static const char *PLAYLIST_DIR = "/data/UserData/move-anything/dj_playlists";
static const int MAX_PLAYLISTS = 32;
static const int MAX_PLAYLIST_TRACKS = 256;

struct playlist_track_t {
    char path[512];
};

struct playlist_entry_t {
    char name[128];
    playlist_track_t *tracks; /* heap-allocated array */
    int  track_count;
};

static playlist_entry_t *s_playlists = nullptr; /* heap-allocated */
static int s_playlist_count = 0;
static bool s_playlists_loaded = false;

static void free_playlists() {
    if (!s_playlists) return;
    for (int i = 0; i < s_playlist_count; i++)
        free(s_playlists[i].tracks);
    free(s_playlists);
    s_playlists = nullptr;
    s_playlist_count = 0;
}

/* Called from get_param (UI thread), never from render_block */
static void scan_playlists() {
    free_playlists();
    s_playlists_loaded = true;
    s_playlists = (playlist_entry_t *)calloc(MAX_PLAYLISTS, sizeof(playlist_entry_t));
    if (!s_playlists) return;

    DIR *dir = opendir(PLAYLIST_DIR);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) && s_playlist_count < MAX_PLAYLISTS) {
        const char *name = ent->d_name;
        int len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".txt") != 0) continue;

        playlist_entry_t *pl = &s_playlists[s_playlist_count];
        int nlen = len - 4;
        if (nlen >= (int)sizeof(pl->name)) nlen = sizeof(pl->name) - 1;
        memcpy(pl->name, name, nlen);
        pl->name[nlen] = '\0';
        pl->track_count = 0;
        pl->tracks = (playlist_track_t *)calloc(MAX_PLAYLIST_TRACKS, sizeof(playlist_track_t));
        if (!pl->tracks) { s_playlist_count++; continue; }

        char fpath[600];
        snprintf(fpath, sizeof(fpath), "%s/%s", PLAYLIST_DIR, name);
        FILE *f = fopen(fpath, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f) && pl->track_count < MAX_PLAYLIST_TRACKS) {
                int ll = strlen(line);
                while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r')) line[--ll] = '\0';
                if (ll == 0) continue;
                snprintf(pl->tracks[pl->track_count].path, 512, "%s", line);
                pl->track_count++;
            }
            fclose(f);
        }
        s_playlist_count++;
    }
    closedir(dir);
}

/* Per-deck get_param */
static int get_deck_param(deck_t *dk, const char *key, char *buf, int buf_len) {
    /* Flush deferred cue saves on UI poll (not in set_param hot path) */
    if (dk->cues_dirty) { save_cues(dk); dk->cues_dirty = 0; }

    if (strcmp(key, "playing") == 0) return snprintf(buf, buf_len, "%d", dk->playing);
    if (strcmp(key, "pitch_semitones") == 0) return snprintf(buf, buf_len, "%d", dk->pitch_semitones);
    if (strcmp(key, "speed_pct") == 0) return snprintf(buf, buf_len, "%d", dk->speed_pct);
    if (strcmp(key, "bpm") == 0) {
        float eff = dk->bpm * (dk->speed_pct / 100.0f) * (dk->vinyl_speed_pct / 100.0f);
        return snprintf(buf, buf_len, "%.1f", eff);
    }
    if (strcmp(key, "detected_bpm") == 0) return snprintf(buf, buf_len, "%.1f", dk->detected_bpm);
    if (strcmp(key, "vinyl_speed") == 0) return snprintf(buf, buf_len, "%d", dk->vinyl_speed_pct);
    if (strcmp(key, "total_frames") == 0) return snprintf(buf, buf_len, "%d", dk->max_frames);
    if (strcmp(key, "is_mod") == 0) return snprintf(buf, buf_len, "%d", dk->is_mod);
    if (strcmp(key, "loaded_file") == 0) return snprintf(buf, buf_len, "%s", dk->loaded_file);

    if (strcmp(key, "play_pos") == 0) {
        if (dk->max_frames > 0) {
            float frac = (float)(dk->req.position / (double)dk->max_frames);
            return snprintf(buf, buf_len, "%.4f", std::max(0.0f, std::min(1.0f, frac)));
        }
        return snprintf(buf, buf_len, "0.0");
    }

    /* stem_name_N, stem_loaded_N, stem_mute_N, stem_vol_N — parse index directly */
    if (strncmp(key, "stem_", 5) == 0) {
        const char *sub = key + 5;
        if (strncmp(sub, "name_", 5) == 0) { int t = sub[5] - '0'; if (t >= 0 && t < NUM_STEMS) return snprintf(buf, buf_len, "%s", dk->tracks[t].file_name); }
        if (strncmp(sub, "loaded_", 7) == 0) { int t = sub[7] - '0'; if (t >= 0 && t < NUM_STEMS) return snprintf(buf, buf_len, "%d", dk->tracks[t].audio_data ? 1 : 0); }
        if (strncmp(sub, "mute_", 5) == 0) { int t = sub[5] - '0'; if (t >= 0 && t < NUM_STEMS) return snprintf(buf, buf_len, "%d", dk->tracks[t].muted); }
        if (strncmp(sub, "vol_", 4) == 0) { int t = sub[4] - '0'; if (t >= 0 && t < NUM_STEMS) return snprintf(buf, buf_len, "%d", (int)(dk->tracks[t].volume * 100.0f)); }
    }

    /* cue_pos_N — parse index directly */
    if (strncmp(key, "cue_pos_", 8) == 0) {
        int c = key[8] - '0';
        if (c >= 0 && c < NUM_CUES) {
            if (dk->cue_frames[c] >= 0 && dk->max_frames > 0)
                return snprintf(buf, buf_len, "%.4f", (float)(dk->cue_frames[c] / (double)dk->max_frames));
            return snprintf(buf, buf_len, "-1");
        }
    }

    if (strcmp(key, "beatgrid_downbeat") == 0) {
        if (dk->beatgrid_downbeat >= 0 && dk->max_frames > 0)
            return snprintf(buf, buf_len, "%.4f", (float)(dk->beatgrid_downbeat / (double)dk->max_frames));
        return snprintf(buf, buf_len, "-1");
    }
    if (strcmp(key, "beatgrid_bpm") == 0) return snprintf(buf, buf_len, "%.2f", dk->beatgrid_bpm);

    if (strcmp(key, "loading") == 0) return snprintf(buf, buf_len, "%d", dk->load_state.load() != 0 ? 1 : 0);
    if (strcmp(key, "filter") == 0) return snprintf(buf, buf_len, "%d", dk->filter_pos);
    if (strcmp(key, "stutter_active") == 0) return snprintf(buf, buf_len, "%d", dk->stutter_active);
    if (strcmp(key, "loop_active") == 0) return snprintf(buf, buf_len, "%d", dk->loop_active);
    if (strcmp(key, "loop_beats_idx") == 0) return snprintf(buf, buf_len, "%d", dk->loop_beats_idx);
    if (strcmp(key, "slip_mode") == 0) return snprintf(buf, buf_len, "%d", dk->slip_mode);

    if (strcmp(key, "waveform") == 0) {
        /* Return 128 hex chars representing peak values 0-15 */
        if (buf_len < 129) return 0;
        for (int i = 0; i < 128; i++) {
            int v = dk->waveform[i] & 0xF;
            buf[i] = (v < 10) ? ('0' + v) : ('a' + v - 10);
        }
        buf[128] = '\0';
        return 128;
    }

    return 0;
}

static int dj_get_param(void *ptr, const char *key, char *buf, int buf_len) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst || !key || !buf || buf_len <= 0) return 0;

    /* Global params */
    if (strcmp(key, "crossfader") == 0) return snprintf(buf, buf_len, "%d", inst->crossfader);
    if (strcmp(key, "a_vol") == 0) return snprintf(buf, buf_len, "%d", (int)(inst->deck_vol[0]*100.0f));
    if (strcmp(key, "b_vol") == 0) return snprintf(buf, buf_len, "%d", (int)(inst->deck_vol[1]*100.0f));
    if (strcmp(key, "master_vol") == 0) return snprintf(buf, buf_len, "%d", (int)(inst->master_vol*100.0f));

    /* Playlist params */
    if (strcmp(key, "playlist_scan") == 0) { scan_playlists(); return snprintf(buf, buf_len, "%d", s_playlist_count); }
    if (strcmp(key, "playlist_count") == 0) {
        if (!s_playlists_loaded) scan_playlists();
        return snprintf(buf, buf_len, "%d", s_playlist_count);
    }
    if (strncmp(key, "playlist_name_", 14) == 0) {
        int idx = atoi(key + 14);
        if (idx >= 0 && idx < s_playlist_count) return snprintf(buf, buf_len, "%s", s_playlists[idx].name);
        return 0;
    }
    if (strncmp(key, "playlist_track_count_", 20) == 0) {
        int idx = atoi(key + 20);
        if (idx >= 0 && idx < s_playlist_count) return snprintf(buf, buf_len, "%d", s_playlists[idx].track_count);
        return 0;
    }
    if (strncmp(key, "playlist_track_", 15) == 0) {
        /* Format: playlist_track_N_M */
        int n = -1, m = -1;
        if (sscanf(key + 15, "%d_%d", &n, &m) == 2) {
            if (n >= 0 && n < s_playlist_count && m >= 0 && m < s_playlists[n].track_count
                && s_playlists[n].tracks)
                return snprintf(buf, buf_len, "%s", s_playlists[n].tracks[m].path);
        }
        return 0;
    }

    /* Deck-prefixed */
    if (key[0] == 'a' && key[1] == '_') return get_deck_param(&inst->decks[0], key+2, buf, buf_len);
    if (key[0] == 'b' && key[1] == '_') return get_deck_param(&inst->decks[1], key+2, buf, buf_len);

    return 0;
}

static int dj_get_error(void *ptr, char *buf, int buf_len) {
    (void)ptr; (void)buf; (void)buf_len; return 0;
}

static void dj_render_block(void *ptr, int16_t *out_lr, int frames) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst) { memset(out_lr, 0, frames * 2 * sizeof(int16_t)); return; }

    /* Check if any background loads have completed — MUST complete before
     * render starts, since it swaps track data the renderer reads. */
    check_load_complete(&inst->decks[0]);
    check_load_complete(&inst->decks[1]);

    /* Parallel deck rendering: deck B on worker thread, deck A on this thread.
     * The two decks share NO state — separate stretchers, separate audio buffers,
     * separate output ring buffers. Cache-line aligned render_buf_a/b prevent
     * false sharing between cores. */
    render_worker_t *w = inst->worker;
    if (w) {
        /* Kick deck B to worker thread */
        pthread_mutex_lock(&w->mutex);
        w->dk = &inst->decks[1];
        w->out_buf = inst->render_buf_b;
        w->frames = frames;
        w->done = 0;
        w->has_work = 1;
        pthread_cond_signal(&w->cond_work);
        pthread_mutex_unlock(&w->mutex);

        /* Render deck A on this thread (runs concurrently with deck B) */
        render_deck(&inst->decks[0], inst->render_buf_a, frames);

        /* Wait for deck B to finish */
        pthread_mutex_lock(&w->mutex);
        while (!w->done)
            pthread_cond_wait(&w->cond_done, &w->mutex);
        pthread_mutex_unlock(&w->mutex);
    } else {
        /* Fallback: sequential rendering if worker thread failed to start */
        render_deck(&inst->decks[0], inst->render_buf_a, frames);
        render_deck(&inst->decks[1], inst->render_buf_b, frames);
    }

    /* Crossfader mixing: precompute combined gain factors.
     * 0=all A, 50=center (both full), 100=all B. */
    const float xf = inst->crossfader * 0.01f;
    const float ga_mv = std::min(1.0f, 2.0f * (1.0f - xf)) * inst->deck_vol[0] * inst->master_vol;
    const float gb_mv = std::min(1.0f, 2.0f * xf) * inst->deck_vol[1] * inst->master_vol;

    const float *__restrict__ a = inst->render_buf_a;
    const float *__restrict__ b = inst->render_buf_b;
    const int n = frames * 2;
    for (int i = 0; i < n; i++) {
        float s = a[i] * ga_mv + b[i] * gb_mv;
        /* Branchless clamp */
        s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        out_lr[i] = (int16_t)(s * 32767.0f);
    }

    /* Flush deferred frees outside the hot render path.
     * This runs after audio is written, so any malloc lock contention
     * doesn't affect this block's latency. */
    flush_deferred_frees();
}

/* ------------------------------------------------------------------ */
/*  Static API table and entry point                                  */
/* ------------------------------------------------------------------ */

static plugin_api_v2_t s_api = {
    /* api_version     */ 2,
    /* create_instance */ dj_create,
    /* destroy_instance*/ dj_destroy,
    /* on_midi         */ dj_on_midi,
    /* set_param       */ dj_set_param,
    /* get_param       */ dj_get_param,
    /* get_error       */ dj_get_error,
    /* render_block    */ dj_render_block,
};

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    s_host = host;
    host_log("[dj] move_plugin_init_v2 called (dual-deck)");
    return &s_api;
}
