#!/usr/bin/env bash
# Build DJ Deck module for Move Anything (ARM64)
# Copyright (c) 2026 DJ Hard Rich
# Licensed under CC BY-NC-SA 4.0
#
# Cross-compiles Bungee, libxmp, minimp3, fdk-aac, dr_flac, and the DJ DSP plugin via Docker.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
# Set DISABLE_LIBXMP=1 to build without MOD file support.
# Set DISABLE_MP3=1 to build without MP3 support.
# Set DISABLE_M4A=1 to build without M4A/AAC support.
# Set DISABLE_FLAC=1 to build without FLAC support.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="dj"
IMAGE_NAME="move-anything-dj-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== DJ Deck Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        -e DISABLE_LIBXMP="${DISABLE_LIBXMP:-}" \
        -e DISABLE_MP3="${DISABLE_MP3:-}" \
        -e DISABLE_M4A="${DISABLE_M4A:-}" \
        -e DISABLE_FLAC="${DISABLE_FLAC:-}" \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

# --- Fetch dependencies (if not already present) ---
# Allow git to work in mounted volumes
git config --global --get safe.directory "$REPO_ROOT" &>/dev/null || \
    git config --global --add safe.directory "$REPO_ROOT" 2>/dev/null || true

fetch_dep() {
    local dir="$1" url="$2"
    if [ ! -d "$dir/.git" ] && [ ! -f "$dir/.git" ]; then
        echo "Fetching $dir..."
        git clone --depth 1 "$url" "$dir"
    fi
}

fetch_dep src/dsp/bungee   https://github.com/bungee-audio-stretch/bungee.git
fetch_dep src/dsp/libxmp    https://github.com/libxmp/libxmp.git
fetch_dep src/dsp/minimp3   https://github.com/lieff/minimp3.git
fetch_dep src/dsp/fdk-aac   https://github.com/mstorsjo/fdk-aac.git

# Bungee has its own submodules (eigen, pffft, cxxopts)
if [ -f "src/dsp/bungee/.gitmodules" ] && [ ! -d "src/dsp/bungee/submodules/eigen/.git" ]; then
    echo "Fetching Bungee submodules..."
    git -C src/dsp/bungee submodule update --init --recursive --depth 1
fi

echo "=== Building DJ Deck Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build/bungee build/libxmp build/fdk-aac
mkdir -p "dist/$MODULE_ID"

BUNGEE_DIR=src/dsp/bungee
LIBXMP_DIR=src/dsp/libxmp
MINIMP3_DIR=src/dsp/minimp3
FDKAAC_DIR=src/dsp/fdk-aac

# --- Step 1: Build pffft (static) ---
echo "Compiling pffft..."
${CROSS_PREFIX}gcc -O3 -fPIC -ffast-math -fno-finite-math-only -fno-exceptions \
    -c "$BUNGEE_DIR/submodules/pffft/pffft.c" -o build/bungee/pffft.o
${CROSS_PREFIX}gcc -O3 -fPIC -ffast-math -fno-finite-math-only -fno-exceptions \
    -c "$BUNGEE_DIR/submodules/pffft/fftpack.c" -o build/bungee/fftpack.o

# --- Step 2: Build Bungee sources (static) ---
echo "Compiling Bungee library..."
for src in $BUNGEE_DIR/src/*.cpp; do
    obj="build/bungee/$(basename "$src" .cpp).o"
    ${CROSS_PREFIX}g++ -O3 -fPIC -std=c++20 -fwrapv \
        -I"$BUNGEE_DIR/submodules/eigen" \
        -I"$BUNGEE_DIR/submodules" \
        -I"$BUNGEE_DIR" \
        '-DBUNGEE_VISIBILITY=__attribute__((visibility("default")))' \
        -DBUNGEE_SELF_TEST=0 \
        -Deigen_assert=BUNGEE_ASSERT1 \
        -DEIGEN_DONT_PARALLELIZE=1 \
        '-DBUNGEE_VERSION="0.0.0"' \
        -c "$src" -o "$obj"
done

echo "Creating libbungee.a..."
${CROSS_PREFIX}ar rcs build/bungee/libbungee.a build/bungee/*.o

# --- Step 3: Build libxmp-lite (if available and not disabled) ---
LIBXMP_FLAGS=""
LIBXMP_LIB=""

if [ -z "$DISABLE_LIBXMP" ] && [ -d "$LIBXMP_DIR/src" ]; then
    echo "Compiling libxmp-lite..."

    # libxmp-lite source files (subset of full libxmp for MOD/XM/IT/S3M)
    LIBXMP_SRCS="
        src/control.c src/dataio.c src/effects.c src/filter.c
        src/flow.c src/format.c src/hio.c src/lfo.c src/load.c
        src/load_helpers.c src/memio.c src/misc.c src/mix_all.c
        src/mix_paula.c src/mixer.c src/period.c src/player.c
        src/read_event.c src/scan.c src/smix.c src/virtual.c
        src/filetype.c src/rng.c src/md5.c src/miniz_tinfl.c
        src/extras.c src/hmn_extras.c src/med_extras.c
        src/far_extras.c
        src/loaders/common.c src/loaders/mod_load.c
        src/loaders/xm_load.c src/loaders/s3m_load.c
        src/loaders/it_load.c src/loaders/itsex.c
        src/loaders/sample.c
    "

    for src in $LIBXMP_SRCS; do
        srcfile="$LIBXMP_DIR/$src"
        if [ ! -f "$srcfile" ]; then
            echo "  Warning: $srcfile not found, skipping"
            continue
        fi
        obj="build/libxmp/$(basename "$src" .c).o"
        ${CROSS_PREFIX}gcc -O2 -fPIC \
            -I"$LIBXMP_DIR/include" \
            -I"$LIBXMP_DIR/src" \
            -DLIBXMP_CORE_PLAYER \
            -c "$srcfile" -o "$obj" 2>/dev/null || true
    done

    # Check if we got any objects
    if ls build/libxmp/*.o &>/dev/null 2>&1; then
        echo "Creating libxmp-lite.a..."
        ${CROSS_PREFIX}ar rcs build/libxmp/libxmp-lite.a build/libxmp/*.o
        LIBXMP_FLAGS="-DHAS_LIBXMP -I$LIBXMP_DIR/include"
        LIBXMP_LIB="build/libxmp/libxmp-lite.a"
        echo "libxmp-lite built successfully (MOD support enabled)"
    else
        echo "Warning: libxmp build produced no objects (MOD support disabled)"
    fi
else
    if [ -n "$DISABLE_LIBXMP" ]; then
        echo "libxmp disabled by DISABLE_LIBXMP (MOD support disabled)"
    else
        echo "libxmp not found at $LIBXMP_DIR (MOD support disabled)"
        echo "  Run: git submodule update --init --recursive"
    fi
fi

# --- Step 4: MP3 support (minimp3, header-only) ---
MP3_FLAGS=""
if [ -z "$DISABLE_MP3" ] && [ -f "$MINIMP3_DIR/minimp3_ex.h" ]; then
    MP3_FLAGS="-DHAS_MP3 -I$MINIMP3_DIR"
    echo "MP3 support: ENABLED (minimp3, header-only)"
else
    if [ -n "$DISABLE_MP3" ]; then
        echo "MP3 disabled by DISABLE_MP3"
    else
        echo "minimp3 not found at $MINIMP3_DIR (MP3 support disabled)"
    fi
fi

# --- Step 5: FLAC support (dr_flac, header-only) ---
FLAC_FLAGS=""
if [ -z "$DISABLE_FLAC" ] && [ -f "src/dsp/dr_flac.h" ]; then
    FLAC_FLAGS="-DHAS_FLAC -Isrc/dsp"
    echo "FLAC support: ENABLED (dr_flac, header-only)"
else
    if [ -n "$DISABLE_FLAC" ]; then
        echo "FLAC disabled by DISABLE_FLAC"
    else
        echo "dr_flac.h not found in src/dsp/ (FLAC support disabled)"
    fi
fi

# --- Step 6: M4A/AAC support (fdk-aac decoder) ---
M4A_FLAGS=""
M4A_LIB=""
if [ -z "$DISABLE_M4A" ] && [ -d "$FDKAAC_DIR/libAACdec" ]; then
    echo "Compiling fdk-aac decoder..."

    FDKAAC_INCLUDES="\
        -I$FDKAAC_DIR/libAACdec/include \
        -I$FDKAAC_DIR/libFDK/include \
        -I$FDKAAC_DIR/libSYS/include \
        -I$FDKAAC_DIR/libMpegTPDec/include \
        -I$FDKAAC_DIR/libSBRdec/include \
        -I$FDKAAC_DIR/libPCMutils/include \
        -I$FDKAAC_DIR/libDRCdec/include \
        -I$FDKAAC_DIR/libArithCoding/include \
        -I$FDKAAC_DIR/libSACdec/include \
        -I$FDKAAC_DIR/libSACenc/include"

    FDKAAC_SRCS=""
    # Collect all decoder .cpp files
    for dir in libFDK/src libSYS/src libMpegTPDec/src libAACdec/src \
               libSBRdec/src libPCMutils/src libDRCdec/src libArithCoding/src \
               libSACdec/src; do
        if [ -d "$FDKAAC_DIR/$dir" ]; then
            for f in "$FDKAAC_DIR/$dir"/*.cpp; do
                [ -f "$f" ] && FDKAAC_SRCS="$FDKAAC_SRCS $f"
            done
        fi
    done

    FDKAAC_OBJ_COUNT=0
    for src in $FDKAAC_SRCS; do
        obj="build/fdk-aac/$(basename "$src" .cpp).o"
        ${CROSS_PREFIX}g++ -O2 -fPIC -std=c++11 \
            $FDKAAC_INCLUDES \
            -c "$src" -o "$obj" 2>/dev/null || {
                echo "  Warning: failed to compile $(basename "$src"), skipping"
                continue
            }
        FDKAAC_OBJ_COUNT=$((FDKAAC_OBJ_COUNT + 1))
    done

    if [ "$FDKAAC_OBJ_COUNT" -gt 0 ]; then
        echo "Creating libfdk-aac.a ($FDKAAC_OBJ_COUNT objects)..."
        ${CROSS_PREFIX}ar rcs build/fdk-aac/libfdk-aac.a build/fdk-aac/*.o
        M4A_FLAGS="-DHAS_M4A -I$FDKAAC_DIR/libAACdec/include -I$FDKAAC_DIR/libSYS/include -I$FDKAAC_DIR/libFDK/include"
        M4A_LIB="build/fdk-aac/libfdk-aac.a"
        echo "fdk-aac built successfully (M4A/AAC support enabled)"
    else
        echo "Warning: fdk-aac build produced no objects (M4A support disabled)"
    fi
else
    if [ -n "$DISABLE_M4A" ]; then
        echo "M4A disabled by DISABLE_M4A"
    else
        echo "fdk-aac not found at $FDKAAC_DIR (M4A support disabled)"
    fi
fi

# --- Step 7: Compile and link DJ plugin ---
echo "Compiling DJ plugin..."
${CROSS_PREFIX}g++ -O3 -shared -fPIC -std=c++20 \
    -I"$BUNGEE_DIR" \
    $LIBXMP_FLAGS \
    $MP3_FLAGS \
    $FLAC_FLAGS \
    $M4A_FLAGS \
    src/dsp/dj_plugin.cpp \
    -o build/dsp.so \
    build/bungee/libbungee.a \
    $LIBXMP_LIB \
    $M4A_LIB \
    -lm -lpthread

echo "Verifying build..."
file build/dsp.so

# --- Step 8: Package ---
echo "Packaging..."
cat src/module.json > "dist/$MODULE_ID/module.json"
cat src/ui.js > "dist/$MODULE_ID/ui.js"
cat build/dsp.so > "dist/$MODULE_ID/dsp.so"
chmod +x "dist/$MODULE_ID/dsp.so"

# Create tarball for release
cd dist
tar -czvf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
echo "Tarball: dist/$MODULE_ID-module.tar.gz"
echo "  MOD support:  $([ -n "$LIBXMP_LIB" ] && echo ENABLED || echo DISABLED)"
echo "  MP3 support:  $([ -n "$MP3_FLAGS" ] && echo ENABLED || echo DISABLED)"
echo "  FLAC support: $([ -n "$FLAC_FLAGS" ] && echo ENABLED || echo DISABLED)"
echo "  M4A support:  $([ -n "$M4A_LIB" ] && echo ENABLED || echo DISABLED)"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
