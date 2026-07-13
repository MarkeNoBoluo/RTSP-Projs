# RTSP-Pusher

Windows RTSP live streaming pusher — captures desktop screen + system audio, encodes to H.264/AAC, pushes to an RTSP server (e.g. MediaMTX).

- **Language**: C++17
- **Compiler**: MSVC 2017
- **Build**: CMake 3.16+
- **Dependencies**: FFmpeg C API (avcodec/avformat/avdevice/avfilter/swresample/swscale), SDL2 (audio capture + event loop)

## Build

Build trees already exist at `build-x86/` and `build-x64/`. Configure and build:

```bash
# x86 (32-bit, FFmpeg 4.2.x)
cmake -B build-x86 -G "Visual Studio 15 2017" -A Win32
cmake --build build-x86 --config Release

# x64 (64-bit, FFmpeg 7.x)
cmake -B build-x64 -G "Visual Studio 15 2017" -A x64
cmake --build build-x64 --config Release
```

Output binaries land in `bin/MSVC2017_x86_Release/` and `bin/MSVC2017_x64_Release/`. SDL2.dll and FFmpeg DLLs are copied there via post-build commands.

Third-party libraries live in `3rd/SDL2/` and `3rd/FFmpeg/{x86|x64}/`.

## Architecture

### Thread pipeline (5 threads)

```
ScreenCapture ──> VideoRawFrameQueue ──> VideoEncode ──> EncodedPacketQueue ──> RTSPMux ──> RTSP server
                                                              ^
SDLAudioCapture ──> AudioRingBuffer ──> AudioEncode ──────────┘
```

GPU path (x64 only, `--hw-encoder qsv`): ScreenCapture/VideoRawFrameQueue/VideoEncode are replaced by a single `GpuVideoEncodeThread` that uses FFmpeg filter graph (`ddagrab -> hwmap=qsv -> scale_qsv -> h264_qsv`) and feeds directly into EncodedPacketQueue.

| Thread | Class | Role |
|--------|-------|------|
| Screen capture | `ScreenCaptureThread` | gdigrab → raw BGRA frames → VideoRawFrameQueue |
| Video encode (CPU) | `VideoEncodeThread` | Raw frames → libx264 → EncodedPacketQueue |
| Video encode (GPU) | `GpuVideoEncodeThread` | ddagrab → QSV scale + h264_qsv → EncodedPacketQueue |
| Audio capture | `SDLAudioCapture` | SDL audio callback → AudioRingBuffer (S16 interleaved) |
| Audio encode | `AudioEncodeThread` | S16 → AAC via FFmpeg → EncodedPacketQueue |
| RTSP mux | `RTSPMuxThread` | Packet dequeue → av_interleaved_write_frame → RTSP output |

### Queues

- **VideoRawFrameQueue** — fixed-size ring buffer of raw `AVFrame*`, blocks producer when full
- **AudioRingBuffer** — byte-level ring buffer (65536 bytes, ~682ms @ 48kHz stereo S16)
- **EncodedPacketQueue** — bounded queue of encoded `AVPacket*` (capacity 60)

Each queue and thread carries a `serial` number to reject stale data after reconnects.

### State machine

`PusherStateMachine` enforces valid transitions:

```
Stopped → Opening → Streaming ⇄ Recovering
                ↓        ↓
              Error  →  Closing → Stopped
```

- `Recovering` / `Reconnecting` — entered transiently during reconnect
- `Error` — unrecoverable failure (e.g. encoder init failure)
- Transitions are atomic compare-exchange; invalid transitions are rejected

### Reconnect lifecycle

`PusherLifecycleManager` handles reconnect with exponential backoff (1s → 8s max):

1. Any thread reports error via `scheduleReconnect(reason)` — pushes `EVENT_RECONNECT` via `SDL_PushEvent`
2. Main thread processes the event in SDL event loop → `doReconnect()`
3. `doReconnect()` calls `stopPipeline()` + `startPipeline()` with new serial
4. Serial increment causes all in-flight data in queues to be rejected (stale guard)

### Main event loop (SDL2-driven)

The main thread runs an SDL2 event loop (`SDL_PollEvent` + `SDL_Delay(1)`), not a dedicated RTSP thread. Three SDL timer callbacks feed user events:

| Timer | Event | Interval | Purpose |
|-------|-------|----------|---------|
| Reconnect | `EVENT_RECONNECT` | One-shot (triggered by worker thread) | Pipeline reconnect |
| Stats | `EVENT_STATS` | 5s | Aggregate + log metrics, write CSV row |
| Duration | `EVENT_DURATION` | One-shot (N seconds) | Auto-exit for test automation |

- `SDL_QUIT` and `ESC`/`q` key trigger graceful shutdown
- `timeBeginPeriod(1)` / `timeEndPeriod(1)` is called to raise the system timer resolution to 1ms

### Monitor enumeration

`enumerateMonitors()` in `main.cpp` uses `EnumDisplayMonitors` + `GetMonitorInfo` (logical rect) and `EnumDisplaySettings` (physical resolution via DEVMODE). Monitors are sorted primary-first, then left-right, top-bottom. Screen index 0 = full virtual desktop; 1+ = specific monitor. When a specific monitor is selected, `captureOffsetX/Y` are auto-set from the monitor's virtual-screen position. The process is set to DPI-aware via `SetProcessDPIAware()` so gdigrab captures native pixels.

### Stats / observability

`PusherStats` aggregates metrics across threads (all atomic):

- Video/audio packet counts, bitrate (windowed)
- Encode latency max (per 5s window), mux write latency max
- Queue depths (raw + encoded), queue max
- VBV underflow count (detected from x264 log messages)
- Audio ring buffer bytes, overflow/underrun counters
- A/V sync offset (`audioPtsMs - videoPtsMs`), PTS error count
- First capture timestamps

Stats are logged every 5 seconds and can be written to CSV via `--stats-csv`.

## CLI arguments

| Flag | Default | Description |
|------|---------|-------------|
| `--url <url>` | `rtsp://192.168.42.116:25544/live` | RTSP push URL |
| `--list-screens` | — | Enumerate monitors and exit |
| `--screen <n>` | `0` | Monitor: 0=all (virtual desktop), 1=primary, 2+ = specific |
| `--capture-size WxH` | `2560x1440` | gdigrab capture resolution |
| `--output-size WxH` | `1920x1080` | Encoded output resolution (CPU path: swscale; GPU path: scale_qsv) |
| `--fps <n>` | `30` | Capture/encode framerate |
| `--bitrate <n>` | `20000` | Video bitrate (kbps) |
| `--maxrate <n>` | `20000` | Max video bitrate (kbps) |
| `--bufsize <n>` | `20000` | VBV buffer size (kbits) |
| `--crf <n>` | `0` | CRF quality; 0 = ABR mode (use bitrate), >0 = CRF mode |
| `--list-audio-devices` | — | List SDL capture devices and exit |
| `--audio-device <name>` | default device | SDL capture device by name |
| `--audio-device-index <n>` | — | SDL capture device by index |
| `--no-audio` | — | Disable audio capture |
| `--transport <tcp\|udp>` | `tcp` | RTSP transport protocol |
| `--hw-encoder <name>` | `off` | Hardware encoder: `auto`, `qsv`, `off` |
| `--list-encoders` | — | List available H.264 encoders and exit |
| `--log <path>` | `rtsp_pusher.log` | Log file path |
| `--stats-csv <path>` | — | Periodic stats CSV output |
| `--duration <seconds>` | — | Auto-exit after N seconds (test automation) |
| `--help` | — | Print usage |

Config defaults are defined in `src/PusherConfig.h`.

## Two video paths

### CPU path (default, x86 + x64)

```
gdigrab (avdevice) → raw BGRA AVFrames → VideoRawFrameQueue(4)
    → VideoEncodeThread: swscale (BGRA→YUV420P) → libx264 → EncodedPacketQueue
```

- Works on both x86 and x64
- `ScreenCaptureThread` uses `dshow` / `gdigrab` avdevice input
- Software scaling via `sws_scale`
- Software encoding via `libx264` with ABR or CRF mode

### GPU QSV path (x64 only, `--hw-encoder qsv`)

```
ddagrab (avdevice) → hwmap=qsv → scale_qsv → h264_qsv → EncodedPacketQueue
```

- X64 only — gated by `#if defined(_WIN64)` in `RTSPusher.cpp:22`
- Single `GpuVideoEncodeThread` replaces capture + raw queue + encode
- Uses FFmpeg filter graph (`avfilter_graph_create_filter`):
  - `ddagrab` source — DirectX Desktop Acquisition
  - `hwmap` — upload to Intel QSV surfaces
  - `scale_qsv` — GPU scaling to output resolution
  - `buffersink` — output encoded packets
- Encoder is `h264_qsv` with `AV_HWDEVICE_TYPE_QSV`
- No intermediate raw frame queue; filter graph handles frame flow internally
- Software encoder path (`libx264`) is skipped entirely when GPU path active

## FFmpeg version difference

| | x86 | x64 |
|---|-----|-----|
| FFmpeg version | 4.2.x | 7.x |
| avcodec DLL | avcodec-58 | avcodec-61 |
| avdevice DLL | avdevice-58 | avdevice-61 |
| avformat DLL | avformat-58 | avformat-61 |
| avfilter DLL | avfilter-7 | avfilter-10 |
| avutil DLL | avutil-56 | avutil-59 |
| swresample DLL | swresample-3 | swresample-5 |
| swscale DLL | swscale-5 | swscale-8 |
| Binaries | `3rd/FFmpeg/x86/` | `3rd/FFmpeg/x64/` |

Key API differences:

- x86 (FFmpeg 4.2): older API, some functions deprecated in 7.x still valid here. `AVCodec*` from `avcodec_find_encoder_by_name()`, `avcodec_register_all()` required.
- x64 (FFmpeg 7.x): `avcodec-61` ABI. Channel layout API changed (`AVChannelLayout` struct instead of `channel_layout` field), codec registration is automatic, some struct fields moved/renamed.

GPU QSV path is x64-only (`_WIN64` guard) because it requires FFmpeg 7.x's `ddagrab` source and updated QSV filter support. The guard is checked in `useGpuQsvPipeline()` in `RTSPusher.cpp`.

## Key source files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Entry point, CLI parsing, SDL event loop, monitor enumeration |
| `src/RTSPusher.cpp` | Pipeline orchestrator — `startPipeline()`, `stopPipeline()`, state transitions |
| `src/PusherConfig.h` | Configuration struct with defaults |
| `src/PusherStateMachine.cpp` | Atomic state transitions (Stopped→Opening→Streaming→Closing→Stopped) |
| `src/PusherLifecycleManager.cpp` | Reconnect with exponential backoff, serial management |
| `src/ScreenCaptureThread.cpp` | gdigrab avdevice capture → VideoRawFrameQueue |
| `src/VideoEncodeThread.cpp` | swscale + libx264 software encoding |
| `src/GpuVideoEncodeThread.cpp` | ddagrab → QSV filter graph → h264_qsv (x64 only) |
| `src/SDLAudioCapture.cpp` | SDL2 audio capture callback → AudioRingBuffer |
| `src/AudioEncodeThread.cpp` | S16 → AAC encoding |
| `src/RTSPMuxThread.cpp` | RTSP mux writing via avformat |
| `src/PusherStats.cpp` | Cross-thread atomic metrics aggregation |
| `src/VideoRawFrameQueue.cpp` | Ring buffer for raw AVFrame pointers, serial-gated |
| `src/EncodedPacketQueue.cpp` | Bounded queue for encoded AVPacket pointers (capacity 60) |
| `src/AudioRingBuffer.cpp` | Byte-level ring buffer (65536 bytes) for audio S16 samples |
| `src/PtsUtils.h` | Wall-clock-to-PTS conversion for video (monotonic guard, 90kHz time_base) |
| `src/HardwareEncoderDetector.cpp` | H.264 encoder enumeration and name resolution |
| `src/logger/Logger.cpp` | File-based logger with timestamps |
| `src/Common.h` | Shared types: PusherState enum, UserEventCode enum, FFmpeg C includes |
| `CMakeLists.txt` | Build config, DLL versioning per architecture |
