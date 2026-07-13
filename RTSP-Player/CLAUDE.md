# CLAUDE.md — RTSP-Player v2

SDL2 + FFmpeg RTSP streaming player. Pure C++17, no Qt dependencies.

## Build & Run

```bash
cmake -S . -B build
cmake --build build --config Release   # or Debug
```

- **Compiler**: MSVC 2017, C++17, `/utf-8`; x86 or x64 auto-detected by CMake (`CMAKE_SIZEOF_VOID_P`). `/SAFESEH:NO` is x86-only.
- **Output**: `bin/MSVC2017_${ARCH}_<Config>/RTSP-Player.exe`
- **Working dir at runtime**: the output directory (DLLs are copied there via POST_BUILD)

```bash
cd bin/MSVC2017_x86_Release   # or bin/MSVC2017_x64_Release
./RTSP-Player.exe --url rtsp://192.168.1.100:554/stream
```

## CLI Usage

```
--url <rtsp_url>          RTSP stream URL (required; default auto-generated if omitted)
--log <path>              Log file path (default: rtsp_player.log)
--csv [path]              CSV stats path (default: rtsp_player_stats.csv; path is optional)
--no-csv                  Disable CSV stats
--fullscreen              Start in fullscreen mode
--winId <hwnd>            Render into existing native window (HWND, hex or decimal;
                          takes priority over --fullscreen; F key disabled)
--transport <tcp|udp>     RTSP transport protocol (default: tcp)
--title <string>          Window title (default: "RTSP Player")
--exit-after <seconds>    Auto-exit after N seconds
--no-audio               Disable audio stream processing (video-only, lowest latency)
--setpts-zero            Low-latency mode: video-only bypass if audio absent,
                         or reduced A+V queue depths (~300ms target)
--hwaccel <auto|dxva2|none> Hardware decode (default: auto; x86 auto falls back to software)
--help                    Show help
```

Positional argument form (`argv[1]` = url, `argv[2]` = log) is deprecated but still accepted.

Keyboard: `Esc` / `Q` = quit, `F` = toggle fullscreen.

## Architecture

Pure single-process, multi-threaded SDL2 application. Main thread runs a 1ms-tick event loop with `SDL_PollEvent`, `videoRefresh()`, and `av_usleep` compensation.

### Component tree

```
main()
 ├── SDL_Init (VIDEO | AUDIO | TIMER)
 ├── SDLRenderer (SDL_Window + SDL_Renderer + SDL_Texture, swscale-based upload)
 ├── RTSPlayer (facade)
 │    ├── PlayerStateMachine — 7-state atomic: Stopped→Connecting→Playing→Recovering→Reconnecting→Error→Closing
 │    ├── StreamLifecycleManager — pipeline owner, owns all threads and FFmpeg contexts
 │    │    ├── DemuxThread      → reads AVPackets, pushes to PacketQueues, triggers reconnect on stream error
 │    │    ├── VideoDecodeThread → pops from video PacketQueue, decodes, writes to VideoFrameQueue
 │    │    ├── AudioWorker      → pops from audio PacketQueue, decodes, swresample→AudioRingBuffer
 │    │    └── SDLAudio         → SDL audio callback reads from AudioRingBuffer, updates AVClock audio PTS
 │    ├── PacketQueue ×2 (video, audio) — deques with condition_variable, serial-aware, peak tracking
 │    ├── VideoFrameQueue (24-slot) — ring buffer between decode and render, ~800ms at 30fps
 │    ├── AVClock — independent video/audio PTS with system timestamps, drift() for A/V sync
 │    └── PlayerStats — all std::atomic metrics, CSV export, SDL timer-driven polling
 └── SDL_AddTimer callbacks push SDL_USEREVENT:
      EVENT_STATS (5s)     → PlayerStats::writeCsvRow()
      EVENT_RECONNECT      → StreamLifecycleManager::doReconnect()
      EVENT_STREAM_EOF     → triggers application exit (pushed from DemuxThread)
```

### Serial mechanism

A generation counter (`m_pktSerial`, `m_generation`) is incremented on reconnect and propagated to every queue and worker. All stale packets/frames with an older serial are discarded, ensuring clean pipeline restart without flushing complexity.

### Reconnect & backoff

On stream error, the DemuxThread callback transitions Playing→Recovering, shuts down the pipeline, and schedules a reconnect via `SDL_AddTimer`. Backoff is exponential: `1000 << min(backoffCount, 3)` → 1s, 2s, 4s, 8s (capped). On success, `backoffCount` resets to 0 and `reconnectCount`/`totalReconnectMs` are updated. On failure, another backoff cycle is scheduled. The `m_generation` counter is incremented on both `open()` and `close()` to invalidate stale work.

### Hardware decode (DXVA2)

Controlled via `--hwaccel` (default: `auto`). In `auto` mode, DXVA2 is attempted on x64 but **skipped on x86** (32-bit process limitation). In `dxva2` mode, failure is fatal. HW frames are transferred back to system memory via `av_hwframe_transfer_data` before entering the frame queue. Metrics: `hwDecodeEnabled`, `hwDecodedFrames`, `hwTransferFailures`, `hwTransferMaxUs`.

### IRenderer abstraction

[SDLRenderer.h](src/SDLRenderer.h) defines `IRenderer` (pure virtual: `init`, `displayFrame`, `setWindowSize`, `destroy`). `RTSPlayer` holds `IRenderer*`, enabling renderer substitution for testing or future backends without touching the player facade.

### Low-latency modes

Two latency profiles, set via `StreamLifecycleManager` parameters:

| Mode | Trigger | Video queue | Audio queue | Ring buffer | Target latency |
|------|---------|-------------|-------------|-------------|----------------|
| Normal | default | 200ms | default | default | ~500-800ms |
| Low-latency A+V | `--setpts-zero` (has audio) | 67ms | 66ms | 60ms | ~300ms |
| Low-latency video-only | `--setpts-zero` (no audio) or `--no-audio` | 67ms | — | — | ~1 frame |

In video-only `--setpts-zero` mode, the render loop drains all but the latest frame before each render, and uses `m_frameTimer = nowSec` (unpaced rendering). The RTSPlayer `m_setptsZero` / `m_lowLatency` flags gate both the queue capacity selection and the render-loop pacing path.

### AVClock & sync

`AVClock` stores `(pts, systemTime)` pairs for video and audio independently:
- **audio**: set by `SDLAudio::sdlCallback` (driven by hardware clock)
- **video**: set by `RTSPlayer::videoRefresh` after rendering a frame
- `drift()` = `audioClock - videoClock` — positive means audio ahead, video must catch up

### Render loop (videoRefresh)

1. Drain frames with stale serial
2. If `--setpts-zero` and no audio: drain all but latest frame (ASAP rendering)
3. Read display-slot PTS; if negative, return
4. If clock not ready: render immediately (first frame)
5. Compute `avDiff = framePts - audioClock`:
   - If far behind (`< -maxAudioLagForQueue`): drop frames from queue to catch up (normal mode: 250ms threshold; low-latency: 80ms)
   - If only 1 frame remains and still behind: enter **fast catch-up** mode (unpaced rendering, `targetDelay = 0.001s`)
   - If ahead (`> SYNC_THRESHOLD`): stretch delay slightly
6. Wait gate: sleep if `targetTime - nowSec > 1ms` (capped at 20ms), then return without rendering
7. Single-frame lateness drop if overdue by `>DROP_THRESHOLD_US` (50ms) and min drop interval elapsed
8. Render via `SDLRenderer::displayFrame()` (swscale → NV12 → SDL_UpdateYUVTexture → SDL_RenderCopy)
9. Update `AVClock` video PTS, advance display slot, update stats

## FFmpeg Usage

- **x86**: FFmpeg 4.2.x dev libs (`3rd/FFmpeg/x86/`), DLLs: avcodec-58, avformat-58, avutil-56, swresample-3, swscale-5
- **x64**: FFmpeg 5+ dev libs (`3rd/FFmpeg/x64/`), DLLs: avcodec-61, avformat-61, avutil-59, swresample-5, swscale-8
- **Headers must be wrapped** in `extern "C" {}` blocks
- `av_log_set_callback(ffmpegLogCallback)` registered in `main.cpp`, routes FFmpeg log messages to `logger::Logger`

## Logging

Singleton `logger::Logger` in `src/logger/Logger.h`:

```cpp
logger::Logger::instance().initLogFile(logPath);  // must init before use
LOG_DEBUG("msg %d", val);
LOG_INFO("msg %s", str);
LOG_WARN("msg");
LOG_ERROR("msg");
logger::Logger::instance().closeLogFile();        // via atexit or explicit
```

Thread-safe via internal `std::mutex`. Log format: `[LEVEL] file.cpp:123 msg`.

## Conventions

- **C++17**, `#pragma once`, `m_` member prefix, 4-space indent
- **UTF-8 with BOM** (MSVC `/utf-8`)
- No Qt, no signals/slots, no QWidget — all rendering via SDL2
- `src/test_qt.cpp` is an orphaned Qt smoke test from v1 era — it is **not compiled** by CMakeLists.txt and should not be deleted

## Further Reference

- [docs/csv-metrics-reference.md](docs/csv-metrics-reference.md) — comprehensive field-by-field reference for CSV stats output (normal ranges, anomaly meanings, known issues)
- [README.md](README.md) — Chinese-language overview

## Directory Layout

```
3rd/
 ├── SDL2/          — SDL2 development libraries (x86)
 └── FFmpeg/
      ├── x86/       — FFmpeg 4.2.x (avcodec-58, avformat-58, ...)
      └── x64/       — FFmpeg 5+   (avcodec-61, avformat-61, ...)
src/
 ├── main.cpp       — entry point, CLI parsing, SDL init, main event loop
 ├── RTSPlayer.*    — facade: videoRefresh, frame timing, sync logic
 ├── StreamLifecycleManager.* — pipeline lifecycle, open/close/reconnect
 ├── PlayerStateMachine.*     — atomic 7-state FSM
 ├── DemuxThread.*            — AVPacket reading thread
 ├── VideoDecodeThread.*      — video decode thread
 ├── AudioWorker.*            — audio decode + resample thread
 ├── AudioRingBuffer.*        — lock-free-ish ring buffer (32 chunks)
 ├── SDLAudio.*               — SDL audio device wrapper
 ├── SDLRenderer.*            — SDL window + texture + swscale
 ├── PacketQueue.*            — thread-safe packet queue with serial support
 ├── VideoFrameQueue.*        — 24-slot frame ring buffer
 ├── AVClock.*                — dual PTS clock for A/V sync
 ├── PlayerStats.*            — atomic metrics + CSV export
 ├── Common.h                 — enums, structs, FFmpeg header wrappers
 ├── test_qt.cpp              — orphaned v1 smoke test (not built)
 └── logger/
      └── Logger.*            — singleton printf-style logger
```
