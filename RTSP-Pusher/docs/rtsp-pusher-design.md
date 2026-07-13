# RTSP-Pusher 设计文档

## 1. 概述

**目标**: 低延迟 RTSP 直播推流器，用于采集 Windows 设备屏幕和设备音频，并推送到流媒体服务器。  
**平台**: Windows, MSVC 2017 x86, C++17, FFmpeg C API (4.2.x), SDL2, CMake  
**依赖**: avdevice / avformat / avcodec / avutil / swresample / swscale, SDL2, winmm  
**核心诉求**: 低延迟、恒定帧率、稳定推流、音视频时间戳连续、异常后可恢复

本项目参考 `RTSP-Player` 的 v2 架构方向：移除 Qt 依赖，使用 SDL2 负责主循环、事件和音频采集，使用 FFmpeg C API 负责屏幕采集、像素转换、编码、封装和 RTSP 网络输出，线程使用 `std::thread` 管理。

目标功能等价于以下 ffmpeg 指令的核心能力，并在此基础上补充音频采集链路：

```powershell
ffmpeg.exe -f gdigrab -framerate 30 -video_size 2560x1440 -i desktop `
  -vf scale=1920x1080,format=yuv420p `
  -c:v libx264 -preset ultrafast -tune zerolatency `
  -profile:v main -level 5.0 -crf 23 `
  -x264-params force-cfr=1:sliced-threads=1 `
  -g 30 -keyint_min 30 -force_key_frames "expr:gte(t,n_forced*1)" `
  -maxrate 4000k -bufsize 7000k -threads 0 `
  -fflags nobuffer -flags +low_delay+global_header `
  -f rtsp -rtsp_transport tcp -muxdelay 0 `
  rtsp://192.168.42.116:25544/7aa22953
```

### 1.1 设计边界

- 屏幕采集使用 FFmpeg `gdigrab`，输入为 `desktop`。
- 视频输出固定为 H.264 / YUV420P / 1920x1080 / 30fps。
- RTSP 输出使用 TCP，`muxdelay=0`，优先低延迟。
- 音频默认使用 SDL2 capture device 获取输入设备 PCM，再交给 FFmpeg 编码。
- SDL2 对“系统播放声”的支持取决于设备是否以录音设备形式暴露；若要稳定采集桌面输出声，需要虚拟回环设备，或后续新增 WASAPI loopback 模块。

---

## 2. 线程模型

```
Main Thread (SDL event loop, ~1ms tick)
  ├── SDL_PollEvent() → quit / key / timer / reconnect
  ├── PusherLifecycleManager::open() / close() / reconnect()
  └── stats timer → PlayerStats 风格 CSV 或日志输出

VideoCapture Thread
  └── av_read_frame(gdigrab) → raw BGRA packet/frame → VideoRawFrameQueue

AudioCapture Callback Thread (SDL 内部管理)
  └── SDL capture callback → AudioRingBuffer(PCM)

VideoEncode Thread
  └── VideoRawFrameQueue → sws_scale(scale + BGRA→YUV420P)
      → libx264 encode → EncodedPacketQueue(video)

AudioEncode Thread
  └── AudioRingBuffer → swresample → AAC encode
      → EncodedPacketQueue(audio)

Mux Thread
  └── EncodedPacketQueue(video/audio)
      → av_interleaved_write_frame()
      → RTSP(TCP) server
```

**关键约束**:
- 主线程只负责 SDL 事件、生命周期调度和统计，不执行阻塞式采集、编码或网络写入。
- 屏幕采集、视频编码、音频编码、RTSP 写包分离，避免网络抖动反压到采集线程。
- 视频时间戳由固定 30fps 时基驱动，模拟 `force-cfr=1`。
- GOP 固定为 30，1 秒一个关键帧，等价于 `-g 30 -keyint_min 30 -force_key_frames expr:gte(t,n_forced*1)`。
- 队列满时优先丢弃旧的未编码视频帧，避免延迟累积；音频不主动丢弃整段数据，优先短暂阻塞或补静音。
- RTSP 输出失败后进入 `Recovering/Reconnecting`，清空队列并递增 serial，旧数据自动失效。

---

## 3. 模块清单

| 模块 | 线程 | 职责 |
|------|------|------|
| `main.cpp` | Main | 初始化日志、SDL、FFmpeg device，解析参数，创建 `RTSPusher` 并运行事件循环 |
| `RTSPusher` | Main | 推流总控，聚合生命周期、队列、统计和配置 |
| `PusherConfig` | Main | 保存采集、编码、RTSP 输出参数 |
| `PusherStateMachine` | 任意 | 管理 Stopped / Opening / Streaming / Recovering / Reconnecting / Error / Closing 状态 |
| `PusherLifecycleManager` | Main / Mux | open/reconnect/flush/close 全生命周期和 serial 管理 |
| `ScreenCaptureThread` | 独立 std::thread | 使用 FFmpeg `gdigrab` 采集桌面帧 |
| `SDLAudioCapture` | Main(init) / SDL 回调 | 打开 SDL 采集设备，回调写入 PCM ring buffer |
| `VideoRawFrameQueue` | 跨线程 | 保存待编码原始视频帧，满时丢旧帧 |
| `AudioRingBuffer` | 跨线程 | 保存 SDL 捕获的 PCM chunk，附带采样时间戳 |
| `VideoEncodeThread` | 独立 std::thread | scale + format 转换，调用 libx264 编码 H.264 |
| `AudioEncodeThread` | 独立 std::thread | PCM 重采样，编码 AAC |
| `EncodedPacketQueue` | 跨线程 | 保存已编码 AVPacket，按 serial 和 stream type 区分 |
| `RTSPMuxThread` | 独立 std::thread | 初始化 RTSP 输出上下文，写 header、interleaved packet、trailer |
| `AVSyncClock` | 跨线程 | 维护音视频 PTS 基准和 wall clock 映射 |
| `PusherStats` | 跨线程 | 采集 fps、编码耗时、推流码率、队列深度、重连次数 |
| `logger/Logger` | 跨线程 | 复用 Player 项目的日志风格 |

---

## 4. 参数映射

| ffmpeg 指令参数 | C++ 实现位置 | 设计取值 |
|----------------|--------------|----------|
| `-f gdigrab` | `ScreenCaptureThread::openInput()` | input format = `gdigrab` |
| `-framerate 30` | `PusherConfig::captureFps` | 30 |
| `-video_size 2560x1440` | `PusherConfig::captureWidth/Height` | 2560x1440 |
| `-i desktop` | `PusherConfig::screenSource` | `desktop` |
| `scale=1920x1080` | `VideoEncodeThread::convertFrame()` | sws_scale 到 1920x1080 |
| `format=yuv420p` | `VideoEncodeThread::convertFrame()` | AV_PIX_FMT_YUV420P |
| `-c:v libx264` | `VideoEncodeThread::openEncoder()` | codec name = `libx264` |
| `-preset ultrafast` | encoder private option | `preset=ultrafast` |
| `-tune zerolatency` | encoder private option | `tune=zerolatency` |
| `-profile:v main` | encoder private option | `profile=main` |
| `-level 5.0` | encoder private option | `level=5.0` |
| `-crf 23` | encoder private option | `crf=23` |
| `force-cfr=1` | `VideoTimestampGenerator` | 固定帧间隔 1/30s |
| `sliced-threads=1` | encoder private option | `x264-params=sliced-threads=1:force-cfr=1` |
| `-g 30` / `-keyint_min 30` | codec context / private option | GOP = 30 |
| `-maxrate 4000k` | rate control | `rc_max_rate=4000000` |
| `-bufsize 7000k` | rate control | `rc_buffer_size=7000000` |
| `-threads 0` | codec context | 自动线程数 |
| `-fflags nobuffer` | input/output option | `fflags=nobuffer` |
| `-flags +low_delay+global_header` | codec context | `AV_CODEC_FLAG_LOW_DELAY` / `AV_CODEC_FLAG_GLOBAL_HEADER` |
| `-f rtsp` | `RTSPMuxThread::openOutput()` | output format = `rtsp` |
| `-rtsp_transport tcp` | output dictionary | `rtsp_transport=tcp` |
| `-muxdelay 0` | output dictionary | `muxdelay=0` |

---

## 5. 关键接口

### 5.1 PusherConfig

```cpp
struct PusherConfig {
    const char* rtspUrl = "rtsp://192.168.42.116:25544/7aa22953";

    const char* screenSource = "desktop";
    int captureWidth  = 2560;
    int captureHeight = 1440;
    int captureFps    = 30;

    int outputWidth  = 1920;
    int outputHeight = 1080;
    int videoBitrate = 4000000;
    int videoMaxrate = 4000000;
    int videoBufsize = 7000000;
    int gopSize      = 30;
    int crf          = 23;

    bool enableAudio = true;
    const char* audioDeviceName = nullptr; // nullptr 表示 SDL 默认采集设备
    int audioSampleRate = 48000;
    int audioChannels   = 2;
    int audioBitrate    = 128000;

    const char* rtspTransport = "tcp";
};
```

### 5.2 状态机

```
Stopped → Opening → Streaming
              │          │
              ↓          ↓
             Error   Recovering → Reconnecting → Streaming
              │          │             │
              └──────────┴─────────────┘
                         ↓
                      Stopped

Streaming / Opening / Reconnecting → Closing → Stopped
```

```cpp
enum class PusherState : int {
    Stopped,
    Opening,
    Streaming,
    Recovering,
    Reconnecting,
    Error,
    Closing
};

class PusherStateMachine {
public:
    PusherState state() const;
    bool transition(PusherState from, PusherState to);
    void forceState(PusherState state);
private:
    std::atomic<int> m_state{static_cast<int>(PusherState::Stopped)};
};
```

### 5.3 ScreenCaptureThread

```cpp
class ScreenCaptureThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    ScreenCaptureThread(const PusherConfig* config,
                        VideoRawFrameQueue* queue,
                        PusherStats* stats);

    bool start(int serial);
    void stop();
    void setErrorCallback(ErrorCallback cb);

private:
    bool openInput();
    void run();
    void closeInput();

    AVFormatContext* m_inputCtx = nullptr;
    int m_videoStreamIndex = -1;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};
    std::atomic<int> m_serial{0};
};
```

**实现要点**:
- 调用 `avdevice_register_all()` 后使用 `av_find_input_format("gdigrab")`。
- `AVDictionary` 设置 `framerate=30`、`video_size=2560x1440`、`draw_mouse=1`。
- `avformat_open_input(&ctx, "desktop", inputFormat, &opts)` 打开桌面输入。
- 采集到的帧保留 serial，写入 `VideoRawFrameQueue`。

### 5.4 SDLAudioCapture

```cpp
class SDLAudioCapture {
public:
    SDLAudioCapture(AudioRingBuffer* ringBuffer, PusherStats* stats);
    ~SDLAudioCapture();

    bool open(const PusherConfig& config, int serial);
    void start();
    void stop();
    void close();
    void setSerial(int serial);

private:
    static void sdlCaptureCallback(void* userdata, Uint8* stream, int len);

    AudioRingBuffer* m_ringBuffer = nullptr;
    PusherStats* m_stats = nullptr;
    SDL_AudioDeviceID m_deviceId = 0;
    SDL_AudioSpec m_obtained{};
    std::atomic<int> m_serial{0};
};
```

**实现要点**:
- `SDL_OpenAudioDevice(deviceName, 1, &desired, &obtained, 0)` 打开采集设备。
- 回调中只复制 PCM 到 `AudioRingBuffer` 并记录采样计数，不做编码。
- 如果没有可用 SDL 采集设备，`enableAudio=true` 时 open 失败并给出明确错误。
- 若目标是采集系统播放声，需要使用系统中暴露为录音设备的虚拟回环设备；这不是 SDL2 在所有机器上自动提供的能力。

### 5.5 VideoEncodeThread

```cpp
class VideoEncodeThread {
public:
    VideoEncodeThread(const PusherConfig* config,
                      VideoRawFrameQueue* inputQueue,
                      EncodedPacketQueue* outputQueue,
                      PusherStats* stats);

    bool start(AVFormatContext* outputCtx, int videoStreamIndex, int serial);
    void stop();
    AVCodecContext* codecContext() const;

private:
    bool openEncoder();
    bool ensureSwsContext(int srcW, int srcH, AVPixelFormat srcFmt);
    bool convertFrame(const RawVideoFrame& src, AVFrame* dst);
    void run();
    void closeEncoder();

    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};
};
```

**实现要点**:
- 编码器优先通过 `avcodec_find_encoder_by_name("libx264")` 获取。
- `time_base={1,30}`，`framerate={30,1}`，`pix_fmt=AV_PIX_FMT_YUV420P`。
- `gop_size=30`，`max_b_frames=0`，`flags` 包含 `LOW_DELAY` 和 `GLOBAL_HEADER`。
- 通过 `av_opt_set` 设置 `preset=ultrafast`、`tune=zerolatency`、`profile=main`、`level=5.0`。
- 每个输出包调用 `av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base)` 后送入 `EncodedPacketQueue`。

### 5.6 AudioEncodeThread

```cpp
class AudioEncodeThread {
public:
    AudioEncodeThread(const PusherConfig* config,
                      AudioRingBuffer* ringBuffer,
                      EncodedPacketQueue* outputQueue,
                      PusherStats* stats);

    bool start(AVFormatContext* outputCtx, int audioStreamIndex, int serial);
    void stop();
    AVCodecContext* codecContext() const;

private:
    bool openEncoder();
    bool ensureResampler(const SDL_AudioSpec& inputSpec);
    void run();
    void closeEncoder();

    AVCodecContext* m_codecCtx = nullptr;
    SwrContext* m_swrCtx = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};
};
```

**实现要点**:
- 默认音频编码为 AAC，采样率 48000，双声道，码率 128kbps。
- SDL 捕获格式与 AAC 编码器格式不一致时使用 `swresample` 转换。
- 音频 PTS 使用累计样本数生成，避免依赖回调触发时间抖动。
- ring buffer 短时为空时可补静音，避免 RTSP 音频时间戳断裂。

### 5.7 RTSPMuxThread

```cpp
class RTSPMuxThread {
public:
    using ErrorCallback = std::function<void(const char*)>;

    RTSPMuxThread(const PusherConfig* config,
                  EncodedPacketQueue* packetQueue,
                  PusherStats* stats);

    bool open();
    bool start(int serial);
    void stop();
    void close();
    AVFormatContext* outputContext() const;

    int videoStreamIndex() const;
    int audioStreamIndex() const;
    void setErrorCallback(ErrorCallback cb);

private:
    bool createStreams();
    bool writeHeader();
    void run();
    void writeTrailer();

    AVFormatContext* m_outputCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};
};
```

**实现要点**:
- `avformat_alloc_output_context2(&ctx, nullptr, "rtsp", rtspUrl)` 创建 RTSP 输出。
- output dictionary 设置 `rtsp_transport=tcp`、`muxdelay=0`。
- `AVFMT_GLOBALHEADER` 存在时，视频和音频编码器都设置 `AV_CODEC_FLAG_GLOBAL_HEADER`。
- 写包前检查 packet serial，不匹配则丢弃。
- `av_interleaved_write_frame` 返回错误时触发生命周期恢复。

---

## 6. 数据流和时间戳

### 6.1 视频链路

```
gdigrab packet/frame
  → RawVideoFrame(serial, frameIndex, captureTimeUs)
  → VideoRawFrameQueue
  → sws_scale(BGRA 2560x1440 → YUV420P 1920x1080)
  → AVFrame.pts = frameIndex
  → libx264
  → AVPacket(H264)
  → RTSPMuxThread
```

视频时基固定为 `1/30`。采集抖动不直接改变编码 PTS，编码线程按递增 `frameIndex` 生成恒定帧率时间戳。若采集队列堆积，丢弃旧帧，只保留最新帧继续编码，从而牺牲画面连续性换取低延迟。

### 6.2 音频链路

```
SDL capture callback
  → AudioRingBuffer(PCM chunk, serial, firstSampleIndex)
  → swresample
  → AVFrame.pts = totalEncodedSamples
  → AAC encoder
  → AVPacket(AAC)
  → RTSPMuxThread
```

音频时基固定为 `1/sample_rate`。音频 PTS 由累计样本数生成，不使用 wall clock 作为 PTS。短时读空时补静音，长时间读空按错误处理进入恢复流程。

### 6.3 音视频同步策略

- 视频是主节奏，固定 30fps。
- 音频按实际采样数推进，Mux 线程按 `av_interleaved_write_frame` 交错写入。
- 不做播放器式 A/V drift 纠偏；推流端只保证时间戳单调、连续和同一启动基准。
- reconnect 后 serial 递增，旧采集帧、旧编码包全部失效。

---

## 7. Back Pressure 和丢弃策略

| 场景 | 策略 |
|------|------|
| 视频原始帧队列满 | 丢弃最旧未编码帧，保留最新画面 |
| 已编码视频包队列满 | 优先触发恢复，不长期积压 |
| 音频 ring buffer 满 | 短暂阻塞写入；超过阈值后记录 overrun |
| 音频 ring buffer 空 | 补静音并记录 underrun |
| RTSP 写包阻塞或失败 | Mux 报错，生命周期进入 Recovering |
| reconnect 发生 | 停线程、flush 队列、serial++、重新 open output |

视频队列的核心目标是“不攒延迟”。音频队列的核心目标是“PTS 连续”。两者策略不同，避免为了完整性牺牲直播实时性。

---

## 8. 生命周期

### 8.1 正常启动

```
main()
  → SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS)
  → avdevice_register_all()
  → RTSPusher::open(config)
  → PusherLifecycleManager::open()
      → open mux output
      → create video/audio streams
      → open encoders
      → write RTSP header
      → start ScreenCaptureThread
      → start SDLAudioCapture
      → start VideoEncodeThread / AudioEncodeThread
      → start RTSPMuxThread
  → state = Streaming
```

### 8.2 正常停止

```
RTSPusher::close()
  → state = Closing
  → stop capture
  → stop encoders
  → stop mux
  → write trailer if output header was written
  → free codec / format / sws / swr resources
  → flush queues
  → state = Stopped
```

### 8.3 异常恢复

```
ScreenCapture / AudioCapture / Encode / Mux 任一模块报错
  → PusherLifecycleManager::scheduleReconnect()
  → SDL_USEREVENT(EVENT_RECONNECT)
  → doReconnect()
      → state = Recovering
      → shutdownPipeline()
      → serial++
      → sleep backoff
      → reopen pipeline
      → state = Streaming
```

指数退避建议与 Player 项目保持一致：1s → 2s → 4s → 8s，上限 8s。

---

## 9. 统计和日志

`PusherStats` 参考 `RTSP-Player` 的统计方式，以原子计数器记录运行状态。

| 字段 | 单位 | 含义 |
|------|------|------|
| `videoFramesCaptured` | 帧 | gdigrab 采集帧数 |
| `videoFramesDropped` | 帧 | 原始帧队列满或 serial 失效丢弃帧数 |
| `videoFramesEncoded` | 帧 | H.264 编码成功帧数 |
| `audioBytesCaptured` | 字节 | SDL 捕获 PCM 字节数 |
| `audioFramesEncoded` | 帧 | AAC 编码帧数 |
| `packetsWritten` | 包 | RTSP 写包成功数 |
| `writeErrorCount` | 次 | `av_interleaved_write_frame` 失败次数 |
| `reconnectCount` | 次 | 重连次数 |
| `videoRawQueueDepth` | 帧 | 原始视频队列深度快照 |
| `encodedQueueDepth` | 包 | 已编码包队列深度快照 |
| `bitrateKbps` | kbps | 最近统计窗口输出码率 |
| `encodeVideoMaxUs` | us | 视频编码最大耗时 |
| `muxWriteMaxUs` | us | RTSP 写包最大耗时 |

日志分类建议：
- `capture`: gdigrab / SDL audio capture
- `encode`: x264 / AAC / sws / swr
- `mux`: RTSP open / write / close
- `lifecycle`: state transition / reconnect
- `stats`: 周期性统计

---

## 10. 目录结构

```
RTSP-Pusher/
├── CMakeLists.txt
├── 3rd/
│   ├── SDL2/
│   │   ├── include/
│   │   └── lib/x86/
│   └── FFmpeg/
│       ├── bin/
│       ├── include/
│       └── lib/
├── src/
│   ├── main.cpp
│   ├── RTSPusher.h / .cpp
│   ├── PusherConfig.h
│   ├── PusherStateMachine.h / .cpp
│   ├── PusherLifecycleManager.h / .cpp
│   ├── ScreenCaptureThread.h / .cpp
│   ├── SDLAudioCapture.h / .cpp
│   ├── VideoEncodeThread.h / .cpp
│   ├── AudioEncodeThread.h / .cpp
│   ├── RTSPMuxThread.h / .cpp
│   ├── VideoRawFrameQueue.h / .cpp
│   ├── AudioRingBuffer.h / .cpp
│   ├── EncodedPacketQueue.h / .cpp
│   ├── AVSyncClock.h / .cpp
│   ├── PusherStats.h / .cpp
│   ├── Common.h
│   └── logger/
│       ├── Logger.h
│       └── Logger.cpp
└── docs/
    └── rtsp-pusher-design.md
```

---

## 11. Phase 实现计划

| Phase | 内容 | 模块 | 验收标准 |
|-------|------|------|----------|
| 1 | 工程骨架和参数解析 | CMake, main, PusherConfig, Logger | 程序可启动，能打印配置和版本信息 |
| 2 | 屏幕采集链路 | ScreenCaptureThread, VideoRawFrameQueue | gdigrab 能稳定采集 2560x1440 desktop，统计 fps 接近 30 |
| 3 | 视频编码链路 | VideoEncodeThread, sws_scale, libx264 | 输出 H.264 包，参数与目标 ffmpeg 指令一致 |
| 4 | RTSP 推流链路 | RTSPMuxThread, EncodedPacketQueue | 可向 `rtsp://192.168.42.116:25544/7aa22953` 推送视频流，播放器可拉流观看 |
| 5 | 音频采集与编码 | SDLAudioCapture, AudioRingBuffer, AudioEncodeThread | SDL 采集设备音频并编码 AAC，RTSP 中包含音频流 |
| 6 | 生命周期恢复 | StateMachine, LifecycleManager, serial | RTSP 服务中断后自动重连，旧包不写入新连接 |
| 7 | 生产化打磨 | PusherStats, CSV, 参数化 | 长时间推流稳定，码率、队列、错误可观测 |

---

## 12. 已知风险

- 当前 ffmpeg 指令只包含视频，音频采集设备和格式需要在实现时单独验证。
- SDL2 采集默认面向录音设备；系统播放声采集需要虚拟回环设备或 WASAPI loopback。
- `gdigrab` 在高分辨率屏幕上 CPU 压力较高，2560x1440→1920x1080 scale + x264 ultrafast 需要实测。
- RTSP 服务器是否接受 AAC 音频、H.264 global header、TCP interleaved，需要用目标服务器验证。
- FFmpeg 4.2.x 的 `libx264` 是否随当前 3rd 包启用，需要通过运行时 `avcodec_find_encoder_by_name("libx264")` 验证。
