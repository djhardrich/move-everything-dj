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
static const int    RENDER_BUF_SIZE = 512; /* 256 frames * 2 channels */

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

    int    stutter_active;
    double stutter_start;
    int    stutter_size_frames;

    int    loop_active;
    double loop_start;
    int    loop_size_frames;
    int    loop_beats_idx;

    int    slip_mode;       /* 0=off, 1=on */
    double slip_position;   /* shadow position that always advances */
    int    slip_engaged;    /* currently in a slip action (loop/cue) */

    Bungee::Stretcher<Bungee::Basic> *stretcher;
    Bungee::Request req;
    float *grain_input;
    int    max_grain;

    float *out_buf;
    int    out_count;

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
};

struct instance_t {
    char module_dir[512];
    deck_t decks[NUM_DECKS]; /* 0=A, 1=B */

    float  master_vol;
    int    crossfader;      /* 0=full A, 50=center, 100=full B */
    float  deck_vol[NUM_DECKS];

    float *render_buf_a;
    float *render_buf_b;
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

static float detect_bpm_from_tracks(track_t *tracks, int max_frames) {
    if (max_frames < SAMPLE_RATE * 2) return 0.0f;
    int analyse_frames = std::min(max_frames, SAMPLE_RATE * 30);

    const int FFT_SIZE = 1024;
    const int HOP_SIZE = 512;
    int num_hops = (analyse_frames - FFT_SIZE) / HOP_SIZE;
    if (num_hops < 16) return 0.0f;

    /* Mix all stems to mono */
    float *mono = (float *)calloc(analyse_frames, sizeof(float));
    if (!mono) return 0.0f;
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
    if (!fft) { free(mono); return 0.0f; }

    float *fft_in  = (float *)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float *fft_out  = (float *)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float *fft_work = (float *)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    int num_bins = FFT_SIZE / 2 + 1;
    float *prev_mag = (float *)calloc(num_bins, sizeof(float));
    float *curr_mag = (float *)calloc(num_bins, sizeof(float));
    float *oss = (float *)calloc(num_hops, sizeof(float));

    /* Hann window */
    float *window = (float *)malloc(FFT_SIZE * sizeof(float));
    for (int i = 0; i < FFT_SIZE; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));

    /* Compute spectral flux onset strength signal */
    for (int h = 0; h < num_hops; h++) {
        int offset = h * HOP_SIZE;

        for (int i = 0; i < FFT_SIZE; i++)
            fft_in[i] = mono[offset + i] * window[i];

        pffft_transform_ordered(fft, fft_in, fft_out, fft_work, PFFFT_FORWARD);

        /* pffft ordered real output: [DC, Nyquist, Re1, Im1, Re2, Im2, ...] */
        curr_mag[0] = fabsf(fft_out[0]);
        curr_mag[FFT_SIZE/2] = fabsf(fft_out[1]);
        for (int k = 1; k < FFT_SIZE/2; k++) {
            float re = fft_out[2*k];
            float im = fft_out[2*k+1];
            curr_mag[k] = sqrtf(re*re + im*im);
        }

        /* Spectral flux: sum of positive magnitude differences */
        float flux = 0;
        for (int k = 0; k < num_bins; k++) {
            float diff = curr_mag[k] - prev_mag[k];
            if (diff > 0) flux += diff;
        }
        oss[h] = flux;

        float *tmp = prev_mag;
        prev_mag = curr_mag;
        curr_mag = tmp;
    }

    free(mono);
    free(window);
    free(curr_mag);
    free(prev_mag);
    pffft_aligned_free(fft_in);
    pffft_aligned_free(fft_out);
    pffft_aligned_free(fft_work);
    pffft_destroy_setup(fft);

    /* Autocorrelation of OSS */
    float hps = (float)SAMPLE_RATE / (float)HOP_SIZE; /* hops per second */
    int min_lag = (int)(hps * 60.0f / 200.0f);
    int max_lag = (int)(hps * 60.0f / 60.0f);
    if (max_lag >= num_hops / 2) max_lag = num_hops / 2 - 1;
    if (min_lag < 1) min_lag = 1;

    float *acf = (float *)calloc(max_lag + 1, sizeof(float));
    if (!acf) { free(oss); return 0.0f; }

    for (int lag = min_lag; lag <= max_lag; lag++) {
        float corr = 0;
        int count = num_hops - lag;
        for (int w = 0; w < count; w++)
            corr += oss[w] * oss[w + lag];
        acf[lag] = corr / (float)count;
    }
    free(oss);

    /* Harmonic enhancement: reinforce lags whose 2x and 3x harmonics also correlate */
    float best_score = 0;
    int best_lag = 0;
    for (int lag = min_lag; lag <= max_lag; lag++) {
        float score = acf[lag];
        if (lag * 2 <= max_lag)
            score *= (1.0f + acf[lag * 2]);
        if (lag * 3 <= max_lag)
            score *= (1.0f + acf[lag * 3] * 0.5f);
        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    /* Parabolic interpolation for sub-bin precision */
    float refined_lag = (float)best_lag;
    if (best_lag > min_lag && best_lag < max_lag) {
        float a = acf[best_lag - 1];
        float b = acf[best_lag];
        float c = acf[best_lag + 1];
        float denom = 2.0f * (2.0f * b - a - c);
        if (fabsf(denom) > 1e-12f)
            refined_lag = (float)best_lag + (a - c) / denom;
    }
    free(acf);

    if (refined_lag <= 0) return 0.0f;
    float detected = 60.0f * hps / refined_lag;

    /* Constrain to DJ range */
    while (detected > 170.0f) detected *= 0.5f;
    while (detected < 70.0f)  detected *= 2.0f;

    host_log("[dj] BPM detected: %.1f (spectral flux + harmonic ACF)", detected);
    return detected;
}

static float detect_bpm(deck_t *dk) {
    return detect_bpm_from_tracks(dk->tracks, dk->max_frames);
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

static void save_cues(deck_t *dk) {
    const char *key = deck_song_key(dk);
    if (!key) return;
    char cmd[600]; snprintf(cmd, sizeof(cmd), "mkdir -p %s", CUE_DIR); system(cmd);
    char cpath[600]; cue_file_path(cpath, sizeof(cpath), key);
    FILE *f = fopen(cpath, "w"); if (!f) return;
    for (int c = 0; c < NUM_CUES; c++) fprintf(f, "%.0f\n", dk->cue_frames[c]);
    fclose(f);
}

static void load_cues(deck_t *dk) {
    const char *key = deck_song_key(dk);
    if (!key) return;
    char cpath[600]; cue_file_path(cpath, sizeof(cpath), key);
    FILE *f = fopen(cpath, "r"); if (!f) return;
    for (int c = 0; c < NUM_CUES; c++) {
        char line[64]; if (!fgets(line, sizeof(line), f)) break;
        dk->cue_frames[c] = atof(line);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/*  Load file auto-detect                                             */
/* ------------------------------------------------------------------ */

/* Background loader thread function */
static void *bg_load_thread(void *arg) {
    deck_t *dk = (deck_t *)arg;
    char path[512];
    snprintf(path, sizeof(path), "%s", dk->new_loaded_file);
    int stem_slot = dk->new_stem_slot;

    /* Initialize new_tracks staging area */
    for (int t = 0; t < NUM_STEMS; t++) {
        memset(&dk->new_tracks[t], 0, sizeof(track_t));
        dk->new_tracks[t].volume = dk->tracks[t].volume;
        dk->new_tracks[t].muted = 0;
    }

    bool ok = false;

    if (is_mod_file(path)) {
#ifdef HAS_LIBXMP
        /* Load MOD using a heap-allocated temp deck (deck_t is too large for stack) */
        deck_t *tmp = (deck_t *)calloc(1, sizeof(deck_t));
        if (tmp) {
            for (int t = 0; t < NUM_STEMS; t++) tmp->tracks[t].volume = 1.0f;
            ok = load_mod(tmp, path);
            if (ok) {
                /* Transfer loaded track data to staging area */
                memcpy(dk->new_tracks, tmp->tracks, sizeof(dk->new_tracks));
                /* Clear tmp's pointers so free(tmp) doesn't free the audio data */
                for (int t = 0; t < NUM_STEMS; t++) tmp->tracks[t].audio_data = nullptr;

                dk->new_is_mod = 1;
                dk->new_max_frames = tmp->max_frames;

                /* BPM: use the value extracted by load_mod from the module */
                dk->new_detected_bpm = tmp->bpm > 0 ? tmp->bpm : 120.0f;

                /* Waveform overview */
                compute_waveform(dk->new_waveform, dk->new_tracks, dk->new_max_frames);

                /* Cues */
                for (int c = 0; c < NUM_CUES; c++) dk->new_cue_frames[c] = -1.0;
                char cpath[600]; cue_file_path(cpath, sizeof(cpath), path);
                FILE *f = fopen(cpath, "r");
                if (f) {
                    for (int c = 0; c < NUM_CUES; c++) {
                        char line[64];
                        if (!fgets(line, sizeof(line), f)) break;
                        dk->new_cue_frames[c] = atof(line);
                    }
                    fclose(f);
                }
            }
            free(tmp);
        }
#endif
    } else {
        if (stem_slot < 0 || stem_slot >= NUM_STEMS) stem_slot = 0;

        /* For non-MOD: copy existing tracks to staging (preserving other stems) */
        for (int t = 0; t < NUM_STEMS; t++) {
            /* Don't copy audio_data pointers - the render thread owns those */
            dk->new_tracks[t].audio_data = nullptr;
            dk->new_tracks[t].audio_frames = 0;
            dk->new_tracks[t].duration_secs = 0;
            dk->new_tracks[t].volume = dk->tracks[t].volume;
            dk->new_tracks[t].muted = dk->tracks[t].muted;
            snprintf(dk->new_tracks[t].file_name, sizeof(dk->new_tracks[t].file_name), "%s", dk->tracks[t].file_name);
            snprintf(dk->new_tracks[t].file_path, sizeof(dk->new_tracks[t].file_path), "%s", dk->tracks[t].file_path);
        }

        /* Duplicate existing audio data for non-target stems (skip if prev was MOD) */
        if (!dk->is_mod) {
            for (int t = 0; t < NUM_STEMS; t++) {
                if (t == stem_slot) continue;
                if (dk->tracks[t].audio_data && dk->tracks[t].audio_frames > 0) {
                    int sz = dk->tracks[t].audio_frames * 2 * sizeof(float);
                    dk->new_tracks[t].audio_data = (float *)malloc(sz);
                    if (dk->new_tracks[t].audio_data) {
                        memcpy(dk->new_tracks[t].audio_data, dk->tracks[t].audio_data, sz);
                        dk->new_tracks[t].audio_frames = dk->tracks[t].audio_frames;
                        dk->new_tracks[t].duration_secs = dk->tracks[t].duration_secs;
                    }
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
            /* BPM detection (use lightweight function, no deck_t on stack) */
            dk->new_detected_bpm = detect_bpm_from_tracks(dk->new_tracks, dk->new_max_frames);
            /* Waveform overview */
            compute_waveform(dk->new_waveform, dk->new_tracks, dk->new_max_frames);
            /* Cues */
            for (int c = 0; c < NUM_CUES; c++) dk->new_cue_frames[c] = -1.0;
            const char *key = path;
            char cpath[600]; cue_file_path(cpath, sizeof(cpath), key);
            FILE *f = fopen(cpath, "r");
            if (f) {
                for (int c = 0; c < NUM_CUES; c++) {
                    char line[64];
                    if (!fgets(line, sizeof(line), f)) break;
                    dk->new_cue_frames[c] = atof(line);
                }
                fclose(f);
            }
        }
    }

    if (ok) {
        dk->load_state.store(2); /* ready for swap */
    } else {
        /* Clean up failed load */
        for (int t = 0; t < NUM_STEMS; t++)
            if (dk->new_tracks[t].audio_data) { free(dk->new_tracks[t].audio_data); dk->new_tracks[t].audio_data = nullptr; }
        dk->load_state.store(0); /* back to idle */
    }
    return nullptr;
}

/* Check if background load is done and swap in new data */
static void check_load_complete(deck_t *dk) {
    if (dk->load_state.load() != 2) return;

    /* Stop playback and swap track data */
    dk->playing = 0;
    dk->out_count = 0;

    /* Free old audio data */
    for (int t = 0; t < NUM_STEMS; t++)
        if (dk->tracks[t].audio_data) { free(dk->tracks[t].audio_data); dk->tracks[t].audio_data = nullptr; }

    /* Move new data in */
    memcpy(dk->tracks, dk->new_tracks, sizeof(dk->tracks));
    for (int t = 0; t < NUM_STEMS; t++) dk->new_tracks[t].audio_data = nullptr; /* ownership transferred */

    dk->is_mod = dk->new_is_mod;
    dk->max_frames = dk->new_max_frames;
    dk->detected_bpm = dk->new_detected_bpm;
    dk->bpm = dk->detected_bpm > 0 ? dk->detected_bpm : 120.0f;
    dk->vinyl_speed_pct = 100;
    snprintf(dk->loaded_file, sizeof(dk->loaded_file), "%s", dk->new_loaded_file);
    memcpy(dk->cue_frames, dk->new_cue_frames, sizeof(dk->cue_frames));
    memcpy(dk->waveform, dk->new_waveform, sizeof(dk->waveform));

    reset_stretcher(dk, 0.0);
    dk->load_state.store(0); /* idle */
    host_log("[dj] background load complete: %s", dk->loaded_file);
}

static bool load_file_auto(deck_t *dk, const char *path, int stem_slot) {
    /* If already loading, ignore */
    int expected = 0;
    if (!dk->load_state.compare_exchange_strong(expected, 1)) {
        host_log("[dj] load ignored - already loading");
        return false;
    }

    snprintf(dk->new_loaded_file, sizeof(dk->new_loaded_file), "%s", path);
    dk->new_stem_slot = stem_slot;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024); /* 256KB stack for loaders */
    int rc = pthread_create(&thread, &attr, bg_load_thread, dk);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        host_log("[dj] failed to create load thread: %d", rc);
        dk->load_state.store(0);
        return false;
    }
    host_log("[dj] background load started: %s (stem %d)", path, stem_slot);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Bungee helpers                                                    */
/* ------------------------------------------------------------------ */

static inline double pitch_multiplier(int semitones) {
    return pow(2.0, semitones / 12.0);
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
    int len = chunk.end - chunk.begin;
    int stride = dk->max_grain;
    memset(dk->grain_input, 0, stride * 2 * sizeof(float));

    for (int i = 0; i < len; i++) {
        int src = chunk.begin + i;
        float L = 0, R = 0;
        for (int t = 0; t < NUM_STEMS; t++) {
            track_t *trk = &dk->tracks[t];
            if (trk->muted || !trk->audio_data || src < 0 || src >= trk->audio_frames) continue;
            L += trk->audio_data[src*2+0] * trk->volume;
            R += trk->audio_data[src*2+1] * trk->volume;
        }
        dk->grain_input[i] = L;
        dk->grain_input[stride+i] = R;
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
    dk->load_state.store(0);
    for (int t = 0; t < NUM_STEMS; t++) dk->tracks[t].volume = 1.0f;
    for (int c = 0; c < NUM_CUES; c++) dk->cue_frames[c] = -1.0;
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

    int safety = frames * 4;
    while (dk->out_count < frames && safety-- > 0) {
        double pos = dk->req.position;

        if (dk->stutter_active && dk->stutter_size_frames > 0) {
            double spos = dk->stutter_start;
            if (pos >= spos + dk->stutter_size_frames || pos < spos) {
                double off = fmod(pos - spos, (double)dk->stutter_size_frames);
                if (off < 0) off += dk->stutter_size_frames;
                dk->req.position = spos + off;
                dk->req.reset = true; dk->stretcher->preroll(dk->req);
            }
        } else if (dk->loop_active && dk->loop_size_frames > 0) {
            double lpos = dk->loop_start;
            if (pos >= lpos + dk->loop_size_frames) {
                double off = fmod(pos - lpos, (double)dk->loop_size_frames);
                dk->req.position = lpos + off;
                dk->req.reset = true; dk->stretcher->preroll(dk->req);
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
        for (int i = 0; i < n; i++) {
            dk->out_buf[(dk->out_count+i)*2+0] = output.data[i];
            dk->out_buf[(dk->out_count+i)*2+1] = output.data[output.channelStride+i];
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

    int drain = std::min(dk->out_count, frames);
    for (int i = 0; i < drain; i++) {
        out_lr[i*2+0] = dk->out_buf[i*2+0];
        out_lr[i*2+1] = dk->out_buf[i*2+1];
    }
    if (drain < frames) memset(out_lr + drain*2, 0, (frames-drain)*2*sizeof(float));

    if (drain < dk->out_count)
        memmove(dk->out_buf, dk->out_buf + drain*2, (dk->out_count-drain)*2*sizeof(float));
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

    inst->render_buf_a = (float *)calloc(RENDER_BUF_SIZE, sizeof(float));
    inst->render_buf_b = (float *)calloc(RENDER_BUF_SIZE, sizeof(float));

    host_log("[dj] dual-deck instance created");
    return inst;
}

static void dj_destroy(void *ptr) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst) return;
    destroy_deck(&inst->decks[0]);
    destroy_deck(&inst->decks[1]);
    if (inst->render_buf_a) free(inst->render_buf_a);
    if (inst->render_buf_b) free(inst->render_buf_b);
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

    for (int t = 0; t < NUM_STEMS; t++) {
        char pk[32];
        snprintf(pk, sizeof(pk), "stem_path_%d", t);
        if (strcmp(key, pk) == 0) { load_file_auto(dk, val, t); return; }
        snprintf(pk, sizeof(pk), "stem_mute_%d", t);
        if (strcmp(key, pk) == 0) { dk->tracks[t].muted = atoi(val) ? 1 : 0; return; }
        snprintf(pk, sizeof(pk), "stem_vol_%d", t);
        if (strcmp(key, pk) == 0) { int v=atoi(val); dk->tracks[t].volume = std::max(0,std::min(100,v))/100.0f; return; }
    }

    if (strcmp(key, "playing") == 0) {
        int v = atoi(val);
        if (v && !dk->playing && dk->stretcher && dk->max_frames > 0) {
            dk->out_count = 0; reset_stretcher(dk, dk->req.position);
        }
        dk->playing = v ? 1 : 0;
    }
    else if (strcmp(key, "pitch_semitones") == 0) {
        int v = atoi(val); dk->pitch_semitones = std::max(-12, std::min(12, v));
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
        dk->out_count = 0; reset_stretcher(dk, frac * (double)dk->max_frames);
    }
    else if (strcmp(key, "set_cue") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < NUM_CUES) { dk->cue_frames[n] = dk->req.position; save_cues(dk); }
    }
    else if (strcmp(key, "jump_cue") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < NUM_CUES && dk->cue_frames[n] >= 0.0) {
            dk->out_count = 0; reset_stretcher(dk, dk->cue_frames[n]);
        }
    }
    else if (strcmp(key, "clear_cue") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < NUM_CUES) { dk->cue_frames[n] = -1.0; save_cues(dk); }
    }
    else if (strcmp(key, "stutter_active") == 0) {
        int v = atoi(val);
        if (v && !dk->stutter_active) {
            dk->stutter_start = dk->req.position;
            if (dk->slip_mode) dk->slip_engaged = 1;
        }
        if (!v && dk->stutter_active && dk->slip_mode && dk->slip_engaged) {
            /* Slip snap-back */
            dk->out_count = 0;
            reset_stretcher(dk, dk->slip_position);
            dk->slip_engaged = 0;
        }
        dk->stutter_active = v ? 1 : 0;
    }
    else if (strcmp(key, "stutter_size") == 0) {
        int idx = std::max(0, std::min(NUM_STUTTER_SIZES-1, atoi(val)));
        dk->stutter_size_frames = beats_to_frames(STUTTER_BEATS[idx], dk->bpm);
        if (dk->stutter_size_frames < 64) dk->stutter_size_frames = 64;
    }
    else if (strcmp(key, "loop_active") == 0) {
        int v = atoi(val);
        if (v) {
            dk->loop_start = dk->req.position;
            dk->loop_size_frames = beats_to_frames(LOOP_BEATS[dk->loop_beats_idx], dk->bpm);
            if (dk->loop_size_frames < 128) dk->loop_size_frames = 128;
            if (dk->slip_mode) dk->slip_engaged = 1;
        }
        if (!v && dk->loop_active && dk->slip_mode && dk->slip_engaged) {
            dk->out_count = 0;
            reset_stretcher(dk, dk->slip_position);
            dk->slip_engaged = 0;
        }
        dk->loop_active = v ? 1 : 0;
    }
    else if (strcmp(key, "loop_size") == 0) {
        int idx = std::max(0, std::min(NUM_LOOP_SIZES-1, atoi(val)));
        dk->loop_beats_idx = idx;
        dk->loop_size_frames = beats_to_frames(LOOP_BEATS[idx], dk->bpm);
        if (dk->loop_size_frames < 128) dk->loop_size_frames = 128;
    }
    else if (strcmp(key, "hot_loop") == 0) {
        /* Atomic set-size-and-activate: val = loop index, or -1 to deactivate */
        int idx = atoi(val);
        if (idx < 0) {
            if (dk->loop_active && dk->slip_mode && dk->slip_engaged) {
                dk->out_count = 0;
                reset_stretcher(dk, dk->slip_position);
                dk->slip_engaged = 0;
            }
            dk->loop_active = 0;
        } else {
            idx = std::max(0, std::min(NUM_LOOP_SIZES-1, idx));
            dk->loop_beats_idx = idx;
            dk->loop_size_frames = beats_to_frames(LOOP_BEATS[idx], dk->bpm);
            if (dk->loop_size_frames < 128) dk->loop_size_frames = 128;
            dk->loop_start = dk->req.position;
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

/* Per-deck get_param */
static int get_deck_param(deck_t *dk, const char *key, char *buf, int buf_len) {
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

    for (int t = 0; t < NUM_STEMS; t++) {
        char pk[32];
        snprintf(pk, sizeof(pk), "stem_name_%d", t);
        if (strcmp(key, pk) == 0) return snprintf(buf, buf_len, "%s", dk->tracks[t].file_name);
        snprintf(pk, sizeof(pk), "stem_loaded_%d", t);
        if (strcmp(key, pk) == 0) return snprintf(buf, buf_len, "%d", dk->tracks[t].audio_data ? 1 : 0);
        snprintf(pk, sizeof(pk), "stem_mute_%d", t);
        if (strcmp(key, pk) == 0) return snprintf(buf, buf_len, "%d", dk->tracks[t].muted);
        snprintf(pk, sizeof(pk), "stem_vol_%d", t);
        if (strcmp(key, pk) == 0) return snprintf(buf, buf_len, "%d", (int)(dk->tracks[t].volume * 100.0f));
    }

    for (int c = 0; c < NUM_CUES; c++) {
        char pk[32]; snprintf(pk, sizeof(pk), "cue_pos_%d", c);
        if (strcmp(key, pk) == 0) {
            if (dk->cue_frames[c] >= 0 && dk->max_frames > 0)
                return snprintf(buf, buf_len, "%.4f", (float)(dk->cue_frames[c] / (double)dk->max_frames));
            return snprintf(buf, buf_len, "-1");
        }
    }

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

    /* Check if any background loads have completed */
    check_load_complete(&inst->decks[0]);
    check_load_complete(&inst->decks[1]);

    /* Render each deck to float buffers */
    render_deck(&inst->decks[0], inst->render_buf_a, frames);
    render_deck(&inst->decks[1], inst->render_buf_b, frames);

    /* Crossfader mixing: 0=all A, 50=center (both full), 100=all B */
    float xf = inst->crossfader / 100.0f;
    float ga = std::min(1.0f, 2.0f * (1.0f - xf)) * inst->deck_vol[0];
    float gb = std::min(1.0f, 2.0f * xf) * inst->deck_vol[1];
    float mv = inst->master_vol;

    for (int i = 0; i < frames * 2; i++) {
        float s = (inst->render_buf_a[i] * ga + inst->render_buf_b[i] * gb) * mv;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out_lr[i] = (int16_t)(s * 32767.0f);
    }
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
