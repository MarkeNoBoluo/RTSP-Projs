#!/bin/bash
set -e

# ── Paths ──────────────────────────────────────────────────────────────────────
PROJ_DIR="/home/wddkxg/Git/RTSP-Projs/RTSP-Pusher"
BUILD_DIR="$PROJ_DIR/build-linux"
BIN_DIR="$PROJ_DIR/bin/Release"
EXE_NAME="RTSP-Pusher"
FFMPEG_LIB_DIR="/home/wddkxg/Libs/ffmpeg-4.4.6/lib"
GCC_LIB_DIR="/usr/local/gcc-12.1.0/lib64"

# ── Parse args ─────────────────────────────────────────────────────────────────
CMD="${1:-build}"

# ── Build ──────────────────────────────────────────────────────────────────────
do_build() {
    echo "[build] Cleaning CMake cache..."
    rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
    echo "[build] Configuring..."
    cmake -S "$PROJ_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1
    echo "[build] Building..."
    cmake --build "$BUILD_DIR" --config Release 2>&1
    echo "[build] Done: $BIN_DIR/$EXE_NAME"
}

# ── Deploy ─────────────────────────────────────────────────────────────────────
do_deploy() {
    local DIST_DIR="$PROJ_DIR/dist/$EXE_NAME"
    local TARBALL="$PROJ_DIR/dist/${EXE_NAME}.tar.gz"

    echo "[deploy] Creating deploy package..."

    rm -rf "$DIST_DIR" "$TARBALL"
    mkdir -p "$DIST_DIR"

    # 1. Copy executable
    cp "$BIN_DIR/$EXE_NAME" "$DIST_DIR/"
    echo "[deploy]   Copied executable"

    # 2. Copy FFmpeg shared libs (Pusher uses avdevice and avfilter too)
    cp -a "$FFMPEG_LIB_DIR"/libavcodec.so.*   "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libavformat.so.*  "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libavutil.so.*    "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libavdevice.so.*  "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libavfilter.so.*  "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libswresample.so.* "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libswscale.so.*   "$DIST_DIR/" 2>/dev/null || true
    cp -a "$FFMPEG_LIB_DIR"/libpostproc.so.*  "$DIST_DIR/" 2>/dev/null || true
    echo "[deploy]   Copied FFmpeg libs"

    # 3. Copy SDL2 (dereference symlink to get the real file)
    local sdl2_lib
    sdl2_lib=$(ldd "$BIN_DIR/$EXE_NAME" 2>/dev/null | grep libSDL2 | awk '{print $3}')
    if [ -n "$sdl2_lib" ] && [ -f "$sdl2_lib" ]; then
        cp -L "$sdl2_lib" "$DIST_DIR/"
        echo "[deploy]   Copied SDL2: $(basename "$sdl2_lib")"
    else
        echo "[deploy]   WARNING: SDL2 not found via ldd, skipping"
    fi

    # 4. Copy GCC runtime (custom install, may not exist on target)
    if [ -f "$GCC_LIB_DIR/libstdc++.so.6" ]; then
        cp -a "$GCC_LIB_DIR/libstdc++.so.6" "$DIST_DIR/"
        echo "[deploy]   Copied libstdc++.so.6"
    fi
    if [ -f "$GCC_LIB_DIR/libgcc_s.so.1" ]; then
        cp -a "$GCC_LIB_DIR/libgcc_s.so.1" "$DIST_DIR/"
        echo "[deploy]   Copied libgcc_s.so.1"
    fi

    # 5. Fix RPATH on executable and all bundled .so files to $ORIGIN
    #    This ensures every bundled lib finds its own dependencies within the bundle.
    echo "[deploy]   Setting RPATH to \$ORIGIN on all bundled binaries..."
    for f in "$DIST_DIR/$EXE_NAME" "$DIST_DIR"/*.so*; do
        [ -f "$f" ] || continue
        [ -L "$f" ] && continue
        patchelf --set-rpath '$ORIGIN' "$f" 2>/dev/null || true
    done

    # 6. Create tarball
    cd "$PROJ_DIR/dist"
    tar czf "${EXE_NAME}.tar.gz" "$EXE_NAME"
    echo "[deploy]   Tarball: $TARBALL"

    # 7. Verify
    echo "[deploy]   Contents:"
    tar tzf "${EXE_NAME}.tar.gz" | sed 's/^/       /'

    echo "[deploy] Done. Copy to target and run:"
    echo "         scp $TARBALL user@target:/tmp/"
    echo "         ssh user@target 'cd /tmp && tar xzf ${EXE_NAME}.tar.gz && cd $EXE_NAME && ./$EXE_NAME --url rtsp://...'"
}

# ── Clean ──────────────────────────────────────────────────────────────────────
do_clean() {
    echo "[clean] Removing build and dist..."
    rm -rf "$BUILD_DIR" "$PROJ_DIR/dist"
    echo "[clean] Done"
}

# ── Dispatch ───────────────────────────────────────────────────────────────────
case "$CMD" in
    build)
        do_build
        ;;
    deploy)
        do_build
        do_deploy
        ;;
    clean)
        do_clean
        ;;
    *)
        echo "Usage: $0 {build|deploy|clean}"
        echo "  build   - build only"
        echo "  deploy  - build + create deploy tarball with dependencies"
        echo "  clean   - remove build and dist directories"
        exit 1
        ;;
esac
