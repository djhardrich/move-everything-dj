# CLAUDE.md - DJ Deck Module for Move Anything

## Overview

Overtake module that turns Ableton Move into a standalone CDJ/turntable-style DJ deck.
Designed for use with a physical DJ mixer between multiple Moves (no crossfading).

Two modes:
- **Stems mode**: 4 WAV files sharing a single playback position (thought of as stems)
- **MOD mode**: .MOD/.XM/.IT/.S3M decomposed into per-channel stems via libxmp

All audio passes through a single Bungee stretcher for real-time timestretch/pitchshift.

## Architecture

- `src/dsp/dj_plugin.cpp` - Plugin API v2 DSP engine (C++20)
- `src/ui.js` - Overtake module JavaScript UI
- `src/module.json` - Module metadata (tool with DSP, claims master knob)

Dependencies (git submodules in `src/dsp/`):
- **bungee** - Timestretch/pitchshift library
- **libxmp** - MOD/XM/IT/S3M tracker playback (optional, `HAS_LIBXMP` flag)

## Hardware Mapping

| Control | Function |
|---------|----------|
| Knob 1 (CC 71) | Pitch shift (-12 to +12 semitones) |
| Knob 2 (CC 72) | Speed/timestretch (50-200%) |
| Knobs 3-6 (CC 73-76) | Per-stem volume (0-100%) |
| Knob 7 (CC 77) | BPM |
| Knob 8 / Volume (CC 79) | Master volume (0-100%) |
| Track buttons (CC 43-40) | Stem mute toggle (instant, no re-render) |
| Pads 1-8 (Notes 68-75) | Page-dependent (cues/stutter/loops) |
| Steps 1-3 (Notes 16-18) | Page select: Hot Cue / Stutter / Loop |
| Data knob turn | Scroll file browser |
| Data knob push | Open/select in file browser |
| Play button | Toggle playback |
| Shift + Pad | Clear hot cue (on Hot Cue page) |

## Pages

1. **Hot Cue**: 8 cue points. First press sets, subsequent jumps, Shift clears.
2. **Stutter FX**: Hold pad for stutter (1/32 to 4 beats). Release stops.
3. **Loop**: Pads 1-7 set loop size, Pad 8 toggles loop on/off.

## DSP Parameters

Set: `stem_path_N`, `stem_mute_N`, `stem_vol_N`, `master_vol`, `playing`,
`pitch_semitones`, `speed_pct`, `bpm`, `seek`, `set_cue`, `jump_cue`,
`clear_cue`, `stutter_active`, `stutter_size`, `loop_active`, `loop_size`,
`load_file`

Get: `play_pos`, `playing`, `total_frames`, `master_vol`, `stem_name_N`,
`stem_loaded_N`, `cue_pos_N`, `is_mod`, `loaded_file`, `bpm`,
`pitch_semitones`, `speed_pct`

## Build

```bash
git submodule update --init --recursive
./scripts/build.sh                     # Docker cross-compile (recommended)
DISABLE_LIBXMP=1 ./scripts/build.sh    # Without MOD support
./scripts/install.sh                   # Deploy to Move
```

## MOD Loading

When a MOD file is loaded, libxmp pre-renders each channel to a separate stem buffer.
Track mutes are then instant (just toggling pre-rendered stems, no re-rendering).
MOD BPM is extracted from the module's initial tempo setting.
