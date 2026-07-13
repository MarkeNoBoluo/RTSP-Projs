# RTSP-Player 设计文档

## 1. 概述

**目标**: 低延迟 RTSP 拉流播放器，用于投影场景。  
**平台**: Windows, MSVC 2017 x86, C++17, FFmpeg C API (4.2.x), SDL2, CMake  
**依赖**: avformat / avcodec / avutil / swresample / swscale, SDL2, winmm  
**核心诉求**: 秒开、低延迟、实时交互优先、音视频链路稳定

> **v2 架构变更 (2026-06-10)**: 移除 Qt 依赖，SDL2 替代 Qt Multimedia/OpenGL，std::thread 替代 QThread。详见 `docs/superpowers/plans/2026-06-10-sdl2-migration-plan.md`。
> **当前进度**: v2 SDL2 已全面实现并通过初步测试。详见 `RTSP-Player-Progress-Activity.md`。

---

## 2. 线程模型（4 线程 + SDL 回调）

```
Main Thread (高频 tick ~1ms)
  ├── SDL_PollEvent() → 事件处理 (quit / key / window / timer)
  ├── videoRefresh() → A/V sync → IRenderer::displayFrame()
  │       │                │
  │       │  AVClock.drift() ← audioClock (SDL 回调设置, 精确)
  │       │
  │       ▼
  │  VideoFrameQueue (8 slot + serial) ← VideoDecode Thread
  │
  └── av_usleep(1ms 补偿)

Demux Thread (std::thread) ── av_read_frame() + serial → PacketQueue(video) / PacketQueue(audio)
VideoDecode Thread (std::thread) ── avcodec → VideoFrameQueue (8 slot)
AudioDecode Thread (std::thread) ── avcodec → swresample → AudioRingBuffer (100ms)

SDL Audio Callback Thread (SDL 内部管理)
  └── read AudioRingBuffer → memcpy → SDL stream
  └── setAudioClock(pcm_pts + consumed_samples / sample_rate)
```

**关键约束**：
- 主循环 1ms tick 高频驱动，不依赖 SDL timer 精度
- videoRefresh() 自己判断帧显示时机，不做跨线程调度
- SDL 音频回调只做 memcpy + 设时钟，不解码（解码在独立 AudioDecode Thread）
- IRenderer 抽象接口，当前实现为 SDLRenderer，未来可切 D3D11Renderer
- Serial 机制保证 reconnect 后旧数据自动失效
- Windows 定时器精度：`SDL_Init` 后需调用 `timeBeginPeriod(1)` 将系统时钟粒度从默认 ~15.6ms 提升至 1ms。主循环使用 `av_usleep` 做 1ms 补偿睡眠，退出前调用 `timeEndPeriod(1)` 恢复
- SDLRenderer 实际使用 NV12 纹理格式 + sws_scale (YUV420P→NV12)，非原设计的 SDL_UpdateYUVTexture 直接上传

---

## 3. 模块清单

| 模块 | 线程 | 职责 |
|------|------|------|
| `main.cpp` | Main | SDL 窗口创建 + 高频 tick 主循环 + 事件分发 |
| `RTSPlayer` | Main | 总控（普通类，无 QObject） |
| `PlayerStateMachine` | 任意 | 7 状态机 |
| `StreamLifecycleManager` | 任意 | open/reconnect/flush/close 全生命周期 + serial 管理 |
| `DemuxThread` | 独立 std::thread | `av_read_frame()` → PacketQueue (带 serial) |
| `VideoDecodeThread` | 独立 std::thread | 解码 → VideoFrameQueue (8 slot, 带 serial) |
| `AudioWorker` | 独立 std::thread | 解码 + swresample → AudioRingBuffer (100ms) |
| `PacketQueue` | 跨线程 | 环形缓冲，KeyFrame 感知 Drop，容量 1000ms (video) / 200ms (audio) |
| `VideoFrameQueue` | 跨线程 | 8-Slot 环形缓冲 + serial + mutex |
| `AudioRingBuffer` | 跨线程 | 100ms 环形缓冲 + PTS chunk 追踪 + serial |
| `AVClock` | 跨线程 | VideoClock + AudioClock (pts + systemTime)，drift() |
| `SDLRenderer` | Main | IRenderer 实现：SDL_Window + SDL_Renderer + SDL_Texture(NV12)，sws_scale YUV420P→NV12 |
| `SDLAudio` | Main (init) / SDL 回调 | SDL 音频设备 + 回调注册 |
| `PlayerStats` | 跨线程 | 性能统计（fps / drop / latency / queue / audio / decode timing / CSV） |

---

## 4. 关键接口

### 4.1 PlayerStateMachine (6 状态)

```
Stopped → Connecting → Playing → Recovering → Reconnecting → Playing
              ↓            ↓           ↓                ↓
             Error        Error       Error           Error
              │            │           │                │
    重连预算耗尽:       重连预算>0:    重连预算>0:       重连预算耗尽:
              │            │           │                │
          → Stopped    → Reconnecting → Reconnecting → Stopped
                     重连预算耗尽:    重连预算耗尽:
                          │               │
                      → Stopped       → Stopped
                                      Closing → Stopped
```

```cpp
enum class PlayerState : int { Stopped, Connecting, Playing, Recovering, Reconnecting, Error, Closing };

class PlayerStateMachine {
public:
    PlayerState state() const;
    bool        transition(PlayerState from, PlayerState to);  // CAS
    void        forceState(PlayerState s);
private:
    std::atomic<int> m_state{static_cast<int>(PlayerState::Stopped)};
};
```

### 4.2 VideoFrameQueue（8-Slot + Serial + Mutex）

八个 slot 由 decodeIdx / renderIdx / displayIdx + count 原子计数器管理，使用 mutex 保护并发写入。

```cpp
static constexpr int kSlotCount = 8;

class VideoFrameQueue {
public:
    bool writeFrame(AVFrame* srcFrame, int64_t pts, int serial, PlayerStats* stats = nullptr);

    bool    hasNewFrame() const;
    int     count() const;
    int64_t peekDisplayPts() const;
    int     peekDisplaySerial() const;
    AVFrame* displayFrame();
    void    advanceDisplay();
    void    discardAndAdvance();

    void flush();

private:
    AVFrame*          m_avFrames[kSlotCount];     // 永久 alloc
    VideoFrame        m_slots[kSlotCount];
    int m_decodeIdx{0};
    int m_renderIdx{0};
    int m_displayIdx{0};
    std::atomic<int> m_count{0};
    mutable std::mutex m_mutex;
    int m_width{0}, m_height{0};
};
```

**与 v1 的关键区别**：
- 4 slot → 8 slot（应对 reconnect / burst decode / render stall）
- 去掉了 `waitForNewFrame(cv)`，主循环 1ms tick 直接非阻塞 peek via `hasNewFrame()`
- 新增 serial 字段，reconnect 后旧帧自动失效
- 使用 mutex 保护并发写入，count 用 atomic
- overwrite 策略：写满时 discardAndAdvance 最旧的 pending 帧

### 4.3 PacketQueue（KeyFrame 感知 Drop + serial）

```cpp
struct PacketNode {
    AVPacket* pkt;
    int64_t   durationUs;
    int       serial;
};

class PacketQueue {
public:
    void init(AVRational timeBase, int capacityMs = 200, const char* name = nullptr);
    bool push(AVPacket* pkt, int serial);   // 超容量: 优先 drop P/B, 保 IDR
    bool pop(AVPacket* pkt, int timeoutMs); // 返回 false 时 pkt 未填充
    int  popSerial() const;
    void flush();
    void abort();
    void drainPeak(int& outPeakMs, int& outPeakPkts);  // 获取并重置容量峰值
};
```

**实际配置**: video queue capacity = 1000ms（设计原定 200ms，实际为应对 RTSP 网络抖动增大），audio queue = 200ms。

### 4.4 AudioRingBuffer（替代 AudioPullDevice）

不继承 QIODevice，纯 C++ 环形缓冲（32 个 chunk slot）。

```cpp
class AudioRingBuffer {
public:
    AudioRingBuffer(int bufferMs = 100);

    void setStats(PlayerStats* stats);

    // AudioWorker 解码线程调用
    bool write(const uint8_t* data, int len, double pts, int serial);
    // 满 → condition_variable::wait（替代 v1 的忙等待 QThread::msleep(1)）

    // SDL 音频回调调用
    int  read(uint8_t* dst, int len, double* outPts, int* outChunkOffset);
    // 空 → 返回可用量 (≤ len)，调用方补静音

    void flush();
    void abort();
    int  serial() const;
    void setSerial(int s);

    int currentFillBytes() const;
    void snapshotRingCounters(int& outFillBytes, int& outReadEmpty, int& outWriteBlocked);

private:
    static constexpr int kMaxChunks = 32;
    Chunk m_chunks[kMaxChunks];
    int m_writeIdx = 0;
    int m_readIdx  = 0;
    int m_readOffset = 0;           // 当前 chunk 内已消费字节数
    int m_avail = 0;
    std::atomic<int> m_serial{0};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_abort{false};
};
```

### 4.5 AVClock

```cpp
struct ClockPoint { double pts; int64_t systemTime; };  // systemTime 单位: us

class AVClock {
public:
    void       setVideoClock(double pts);    // 主线程 videoRefresh 渲染后调用
    ClockPoint videoClock() const;

    void       setAudioClock(double pts);    // SDL 音频回调中调用
    ClockPoint audioClock() const;

    bool       isReady() const;              // setVideoClock 首次调用后 true
    bool       hasAudio() const;             // setAudioClock 首次调用后 true
    double     drift() const;                // audio clock - video clock (秒)
    void       reset();

private:
    std::atomic<double>  m_videoPts{0.0};
    std::atomic<int64_t> m_videoSysTime{0};
    std::atomic<bool>    m_videoReady{false};
    std::atomic<double>  m_audioPts{0.0};
    std::atomic<int64_t> m_audioSysTime{0};
};
```

**v2 关键变化**：
- 音频时钟由 SDL 回调设置（而非 AudioWorker 解码时设置）
- 音频时钟计算公式：`pcm_pts + consumed_samples / sample_rate`
- 偏差 ≤ 一次 SDL 回调周期（~20ms），v1 偏差可达 200ms
- `std::atomic<double>` 在 x86 32位平台不保证无锁（取决于编译器），MSVC 通常用 `lock cmpxchg8b` 实现。`drift()` 同时读 video/audio 两个 atomic 值，存在时间窗口内读到 video 旧值 + audio 新值（或反之），偏差 ≤ 一次回调周期（~20ms），在此容忍范围内不追求强一致性，是有意为之

### 4.6 SDLRenderer（IRenderer 实现）

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(int width, int height) = 0;
    virtual void displayFrame(AVFrame* frame) = 0;
    virtual void setWindowSize(int w, int h) = 0;
    virtual void destroy() = 0;
};

class SDLRenderer : public IRenderer {
public:
    SDLRenderer(const char* title, int w, int h);
    ~SDLRenderer() override;

    bool init(int width, int height) override;
    void displayFrame(AVFrame* frame) override;   // sws_scale YUV420P→NV12 → SDL_UpdateNVTexture → RenderCopy → Present
    void setWindowSize(int w, int h) override;
    void destroy() override;

    SDL_Window* window() const { return m_window; }

private:
    void recreateTexture(int width, int height);  // SDL_CreateTexture(NV12, STREAMING)
    void updateDisplayRect();                     // 保持宽高比的 letterbox
    bool ensureSwsContext(int width, int height); // sws_getContext YUV420P→NV12 SWS_FAST_BILINEAR
    int  convertToNV12(AVFrame* src, uint8_t** outPlanes, int* outStrides);

    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture  = nullptr;
    SDL_Rect      m_dstRect;

    SwsContext* m_swsCtx   = nullptr;
    uint8_t*    m_swsBuf   = nullptr;  // Y + UV 平面连续缓冲
    int         m_swsBufW  = 0;
    int         m_swsBufH  = 0;

    int m_texW = 0, m_texH = 0;
    int m_winW = 0, m_winH = 0;
};
```

**与 GLVideoWidget v1 的区别**：
- 无需 OpenGL shader 文件
- 无需 QOpenGLWidget 继承
- CPU sws_scale YUV420P→NV12 + SDL_UpdateNVTexture（NV12 是 SDL 高效纹理格式）
- CPU upload（当前方案），后续可换 D3D11Renderer GPU direct
- SDL_RENDERER_ACCELERATED + SDL_RENDERER_PRESENTVSYNC 标志

### 4.7 SDLAudio（替代 QAudioOutput + AudioPullDevice）

```cpp
class SDLAudio {
public:
    SDLAudio(AudioRingBuffer* ringBuffer, AVClock* clock, PlayerStats* stats);
    ~SDLAudio();

    bool init(int sampleRate, int channels);   // SDL_OpenAudioDevice(desired=48000/2ch/s16)
    void start();                               // SDL_PauseAudioDevice(0)
    void stop();                                // SDL_PauseAudioDevice(1)
    void close();                               // SDL_CloseAudioDevice

private:
    static void sdlCallback(void* userdata, Uint8* stream, int len);

    AudioRingBuffer* m_ringBuffer;
    AVClock*         m_clock;
    PlayerStats*     m_stats;
    SDL_AudioDeviceID m_deviceId = 0;
    int m_sampleRate  = 48000;
    int m_channels    = 2;
    int m_bytesPerSample = 4;  // stereo s16

    // PTS 追踪：累计 consumed samples / sample_rate 推算当前 PTS
    double m_currentPts = 0.0;
    int    m_chunkConsumed = 0;  // 当前 chunk 内已消费字节数
};
```

### 4.8 StreamLifecycleManager（去 QObject）

```cpp
class StreamLifecycleManager {
public:
    using StateCallback  = std::function<void(PlayerState)>;
    using ErrorCallback  = std::function<void(const char*)>;

    StreamLifecycleManager(PlayerStateMachine* sm, PlayerStats* stats,
                           PacketQueue* videoQ, PacketQueue* audioQ,
                           VideoFrameQueue* frameQ, AVClock* clock);

    void setStateCallback(StateCallback cb)  { m_onState = std::move(cb); }
    void setErrorCallback(ErrorCallback cb)  { m_onError = std::move(cb); }

    bool open(const char* url);
    void close();

    PlayerStats*  stats()    const { return m_stats; }
    PlayerState   state()    const;
    class AudioRingBuffer* audioRingBuffer() const { return m_audioRingBuffer; }
    int           pktSerial() const { return m_pktSerial.load(std::memory_order_acquire); }
    void          doReconnect();

private:
    bool initDemux(const char* url);
    bool initDecoders();
    void startThreads();
    void shutdownPipeline();

    void scheduleReconnect();   // SDL_AddTimer → SDL_USEREVENT
    void doReconnect();

    void incrementSerial() { m_pktSerial.fetch_add(1); }

    // ... members ...
    std::atomic<int>  m_pktSerial{0};
    SDLAudio*         m_sdlAudio = nullptr;
};
```

**v2 关键变化**：
- `signals:` → `std::function<>` 回调
- `QTimer` → `SDL_AddTimer` (回调中 `SDL_PushEvent(SDL_USEREVENT)` 回主线程)
- `QAudioOutput` + `AudioPullDevice` → `SDLAudio` + `AudioRingBuffer`
- `GLVideoWidget` → `IRenderer*`
- 新增 `m_pktSerial` 管理 reconnect 安全
- DemuxThread 通过 stream error 回调触发 reconnect 流程
- calcBackoffMs() 指数退避：1s → 2s → 4s → 8s (max)

### 4.9 main.cpp 主循环

```cpp
int main(int argc, char* argv[]) {
    Logger::init();
    av_log_set_callback(ffmpegLogCallback);
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
    timeBeginPeriod(1);

    SDLRenderer renderer("RTSP Player", 1280, 720);
    RTSPlayer player;
    player.setRenderer(&renderer);
    player.open(url);

    // SDL_AddTimer(5000, onStatsTimer, stats) — 每 5s 推送 EVENT_STATS

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // SDL_QUIT / SDL_KEYDOWN(ESC/Q/F) / SDL_WINDOWEVENT_RESIZED
            // SDL_USEREVENT: EVENT_RECONNECT → doReconnect() / EVENT_STATS → writeCsvRow()
        }

        int64_t beforeUs = av_gettime_relative();
        player.videoRefresh();
        int64_t elapsedUs = av_gettime_relative() - beforeUs;
        int64_t sleepUs = 1000 - elapsedUs;
        if (sleepUs > 100) {
            av_usleep((unsigned)sleepUs);
        }
    }

    player.close();
    timeEndPeriod(1);
    SDL_Quit();
}
```

**关键实现细节**：
- 使用 `av_usleep` 而非 `SDL_Delay` 做 1ms 补偿睡眠
- `videoRefresh()` 有 `m_inVideoRefresh` 重入保护
- Stats CSV timer 用 `SDL_AddTimer(5000, onStatsTimer, stats)` 每 5s 触发
- Reconnect 通过 `SDL_AddTimer` + `SDL_USEREVENT(EVENT_RECONNECT)` 回到主线程执行

### 4.10 PlayerStats

```cpp
class PlayerStats {
public:
    // 帧计数器
    std::atomic<int64_t> framesDecoded{0};
    std::atomic<int64_t> framesRendered{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int>     reconnectCount{0};

    // 时序 (us)
    std::atomic<int64_t> lastLatenessUs{0};
    std::atomic<int64_t> maxLatenessUs{0};
    std::atomic<int64_t> renderSkipBurst{0};
    std::atomic<int64_t> totalReconnectMs{0};

    // 音频
    std::atomic<int>     audioUnderruns{0};
    std::atomic<int>     audioOverruns{0};
    std::atomic<int>     videoQueuePeakMs{0};
    std::atomic<int>     audioQueuePeakMs{0};
    std::atomic<int>     videoQueuePeakPkts{0};

    // 解码性能
    std::atomic<int64_t> decodeSendUsMax{0};
    std::atomic<int64_t> decodeReceiveUsMax{0};
    std::atomic<int>     decodeErrorCount{0};

    // FrameQueue
    std::atomic<int>     frameQueueWriteFailures{0};
    std::atomic<int>     frameQueueOverwrites{0};

    // 音频实际计数
    std::atomic<int64_t> audioPacketsReceived{0};
    std::atomic<int64_t> audioFramesDecoded{0};
    std::atomic<int64_t> audioBytesWritten{0};

    // AudioRingBuffer 计数器
    std::atomic<int>     audioRingFillBytes{0};
    std::atomic<int>     audioRingReadEmpty{0};
    std::atomic<int>     audioRingWriteBlocked{0};

    // 渲染间距 (us)
    std::atomic<int64_t> paintIntervalMinUs{0};
    std::atomic<int64_t> paintIntervalMaxUs{0};
    std::atomic<int64_t> paintIntervalSumUs{0};
    std::atomic<int>     paintIntervalCount{0};

    // 渲染耗时 (us)
    std::atomic<int64_t> paintLatencyMinUs{0};
    std::atomic<int64_t> paintLatencyMaxUs{0};
    std::atomic<int64_t> paintLatencySumUs{0};
    std::atomic<int>     paintLatencyCount{0};

    // 单调帧 ID
    std::atomic<uint64_t> frameId{1};
    std::atomic<int64_t>  lastCommitUs{0};
    std::atomic<int64_t>  reconnectStartUs{0};

    // CSV
    void initCsv(const std::string& path);
    void writeCsvRow();           // 每 5s 由 SDL_AddTimer 触发
    void closeCsv();

    void recordPaintInterval(int64_t us);
    void recordPaintLatency(int64_t us);
    void recordQueueDepth(int videoMs, int audioMs);

private:
    std::mutex   m_csvMutex;
    std::ofstream m_csvFile;
    std::string  m_sessionId;
    bool m_csvHeaderWritten = false;
};
```

### 4.10.1 Stats CSV 字段定义

CSV 每 5s 由 `SDL_AddTimer` 触发写入一行，所有计数器为**累积值**，除少数独立快照字段（aRingFillB 等）外不会重置。

| CSV 列 | 对应字段 | 单位 | 含义 |
|--------|----------|------|------|
| `session` | m_sessionId | - | 会话标识，格式 `YYYYMMDD_HHMMSS` |
| `elapsed_s` | av_gettime_relative() | 秒 | 从进程启动至今的 wall clock 时间（us 值/1e6） |
| `vFrmDec` | framesDecoded | 帧 | 累计解码视频帧数（VideoDecodeThread 产出） |
| `vFrmRend` | framesRendered | 帧 | 累计渲染帧数（videoRefresh 实际 display） |
| `vFrmDrop` | framesDropped | 帧 | 累计丢弃帧数（lateness > 50ms 触发单帧 drop） |
| `vDropRate_pct` | 计算: framesDropped/framesDecoded | % | 丢帧率 |
| `aPktIn` | audioPacketsReceived | 包 | 累计收到的音频包数（DemuxThread→AudioQueue→AudioWorker） |
| `aFrmDec` | audioFramesDecoded | 帧 | 累计解码音频帧数（AudioWorker 产出） |
| `aBytesWrit` | audioBytesWritten | 字节 | 累计写入 AudioRingBuffer 的 PCM 字节数 |
| `vQPeakMs` | videoQueuePeakMs | ms | 视频 PacketQueue 的时长峰值（drainPeak 获取后重置） |
| `vQPeakPkt` | videoQueuePeakPkts | 包 | 视频 PacketQueue 的包数峰值 |
| `aQPeakMs` | audioQueuePeakMs | ms | 音频 PacketQueue 的时长峰值 |
| `latAvgMs` | lastLatenessUs / 1000 | ms | 最近一次 videoRefresh 的 lateness 快照（负数=帧超前，正数=帧滞后） |
| `latMaxMs` | maxLatenessUs / 1000 | ms | 累计最大绝对 lateness |
| `skipBurst` | renderSkipBurst | 次 | 最大连续丢帧数（burst tracking，理想值 0~1） |
| `vDecSendMaxUs` | decodeSendMaxUs | us | 解码线程推送帧到 FrameQueue 的最大耗时 |
| `vDecRecvMaxUs` | decodeReceiveMaxUs | us | VideoDecodeThread pop PacketQueue 的最大耗时 |
| `vDecErr` | decodeErrorCount | 次 | 解码错误计数（avcodec_receive_frame 返回非 0 且非 EAGAIN） |
| `fqFail` | frameQueueWriteFailures | 次 | VideoFrameQueue writeFrame 失败计数（队列满且无法 overwrite） |
| `fqOverwrt` | frameQueueOverwrites | 次 | VideoFrameQueue 覆盖旧帧计数（队列满时替换 oldest pending 帧） |
| `aUnder` | audioUnderruns | 次 | SDL 音频回调从 ring buffer 读空次数（≤请求量，补静音） |
| `aOver` | audioOverruns | 次 | AudioRingBuffer write 被拒次数（超出总容量） |
| `aRingFillB` | audioRingFillBytes | 字节 | ring buffer 当前填充量（快照，非累计） |
| `aRingEmptyN` | audioRingReadEmpty | 次 | ring buffer 读空触发次数（累计） |
| `aRingBlockN` | audioRingWriteBlocked | 次 | ring buffer 写满阻塞次数（累计，AudioWorker cv wait） |
| `rndIntvAvgMs` | paintIntervalSum/paintIntervalCount/1000 | ms | 渲染间隔平均值（两次 displayFrame 之间的时间差） |
| `rndIntvMaxMs` | paintIntervalMaxUs/1000 | ms | 渲染间隔最大值 |
| `rndLatAvgMs` | paintLatencySum/paintLatencyCount/1000 | ms | 渲染耗时平均值（displayFrame 自身耗时，含 sws+SDL upload+present） |
| `rndLatMaxMs` | paintLatencyMaxUs/1000 | ms | 渲染耗时最大值 |
| `reconn` | reconnectCount | 次 | 累计重连次数 |
| `reconMs` | totalReconnectMs | ms | 累计重连消耗总时间 |
| `v1stDecUs` | videoFirstDecodeUs | us | 首帧解码时间戳（av_gettime_relative），0 表示未发生 |
| `v1stRendUs` | videoFirstRenderUs | us | 首帧渲染时间戳（av_gettime_relative），0 表示未发生 |
| `a1stDecUs` | audioFirstDecodeUs | us | 首个音频帧解码时间戳 |
| `a1stPlayUs` | audioFirstPlayUs | us | 首个音频帧推入 SDL 设备时间戳 |
| `frameId` | frameId | - | 单调递增帧 ID（每次 render+drop 都 +1，跨 connect 持续增长） |

**注意事项**:
- `latAvgMs` 命名有误导性：实际是最近一次 `videoRefresh()` 的 overdue 值（快照），非平均值。v2 Phase 5 以后 overdue 仅在 drop 时非零
- `fqOverwrt` 高表示渲染消费速度落后于解码产出速度，队列满导致帧被覆盖
- `aRingFillB` 稳定在 44KB-107KB 波动为正常（100ms × 48000Hz × 4B = ~19KB 理论值，实际有缓冲波动）
- `rndIntvAvgMs` 预期 30-36ms (30fps)，偏离说明 pacing 异常或存在队列 overwrite 导致 PTS 跳跃

### 4.11 Common.h

```cpp
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

enum class PlayerState : int {
    Stopped, Connecting, Playing, Recovering, Reconnecting, Error, Closing
};

struct VideoFrame {
    AVFrame* frame = nullptr;
    int64_t  pts   = AV_NOPTS_VALUE;
    int64_t  presentTime = 0;
    int      serial = 0;
};

struct ClockPoint {
    double  pts;
    int64_t systemTime;
};

// SDL_USEREVENT codes — pushed by SDL_AddTimer callbacks, consumed by main loop
enum UserEventCode : int {
    EVENT_RECONNECT = 1,
    EVENT_STATS     = 2,
};
```

---

## 5. 关键工程细节

### 泄漏控制
- 所有 AVFrame 由 VideoFrameQueue 固定池统一管理生命周期
- `av_frame_move_ref` 转移引用，零拷贝
- `av_packet_unref` 在 PacketQueue::pop 消费后调用
- codec context 由 StreamLifecycleManager 独立管理（`avcodec_open2` / `avcodec_free_context`），不跟随 `avformat`

### Back Pressure
- PacketQueue push 满 → drop oldest (优先丢 P/B frame，保 IDR)
- AudioRingBuffer write 满 → condition_variable::wait（v1 是忙等待 QThread::msleep(1)）

### A/V Sync（Phase 5 重构后）

同步模型已于 Phase 5 从经验调参重构为 ffplay 风格的时间驱动模型：

**核心机制**:
- `frameTimer` — 目标显示时间点，仅 render/drop 时推进（非每 tick）
- `compute_target_delay` — 渐进纠偏 (`*0.8 ~ *1.5`)，不一次性补齐 drift
- drfit/lateness 解耦 — avDiff 控制 frameTimer 微调，lateness 控制单帧 drop
- 单帧 drop — 每周期最多 1 帧，间隔 ≥33ms
- delay 钳位 10~100ms（替代旧 5~500ms）
- avDiff 方向: `videoClock - audioClock`（与 ffplay 一致）

**当前实现状态（2026-06-12）**:
- 丢帧率: 46% → **0%**（burst drop 完全解决）
- 渲染稳定性: 无振荡，skipBurst=0
- 渲染速率: ~18fps（待修复 actualDuration 反馈回路）
- avDiff: -0.3~-0.6（视频落后音频），同步纠偏未激活（|avDiff| > DIFF_BOUND=0.15）

**待修复**:
- Audio clock 撕裂读 → avDiff 跳变，同步方向失真
- actualDuration 由 PTS 间隙驱动 → 帧队列 overwrite 导致 pacing 变慢

### Serial 机制
- StreamLifecycleManager 持有 `std::atomic<int> m_pktSerial`
- `open()` / `reconnect()` → `incrementSerial()`
- DemuxThread 每包 stamp serial，decode 线程/渲染检查匹配
- `shutdownPipeline()` → `m_pktSerial++`，所有旧数据自动失效
- 确保 RTSP reconnect 后不会显示上一连接的残留帧

### IRenderer 接口预留
- 当前: SDLRenderer（sws_scale YUV420P→NV12 + SDL_UpdateNVTexture CPU upload）
- 未来: D3D11Renderer（GPU direct YUV upload）
- 接口抽象隔离渲染后端，切换不影响上层逻辑

### 秒开策略
- `avformat_open_input` 设置 `probesize` / `analyzeduration` 为较小值以加速首帧
- PacketQueue 容量 1000ms (video) / 200ms (audio)，不强缓冲
- 首帧立即渲染
- **当前问题**: 实际 probesize=5MB / analyzeduration=5s 过大，导致首帧 ~7.3s。需降低至 ~32KB 级别

---

## 6. 目录结构

```
RTSP-Player/
├── CMakeLists.txt
├── RTSP-Player-Progress-Activity.md       # 实施进度跟进
├── 3rd/
│   ├── SDL2/
│   │   ├── include/          # SDL2 头文件
│   │   └── lib/x86/          # SDL2.lib + SDL2.dll
│   └── FFmpeg/
│       ├── bin/              # avcodec-58.dll, avformat-58.dll, avutil-56.dll,
│       │                     # swresample-3.dll, swscale-5.dll (x86)
│       ├── include/          # FFmpeg 头文件
│       └── lib/              # avcodec.lib 等 (x86)
├── src/
│   ├── main.cpp              # SDL 窗口 + 主循环 + 事件分发
│   ├── RTSPlayer.h / .cpp
│   ├── StreamLifecycleManager.h / .cpp
│   ├── PlayerStateMachine.h / .cpp
│   ├── DemuxThread.h / .cpp
│   ├── VideoDecodeThread.h / .cpp
│   ├── AudioWorker.h / .cpp
│   ├── SDLRenderer.h / .cpp
│   ├── SDLAudio.h / .cpp
│   ├── AudioRingBuffer.h / .cpp
│   ├── PacketQueue.h / .cpp
│   ├── VideoFrameQueue.h / .cpp
│   ├── AVClock.h / .cpp
│   ├── PlayerStats.h / .cpp
│   ├── Common.h
│   └── logger/
│       ├── Logger.h
│       └── Logger.cpp
└── docs/
    ├── rtsp-player-design.md             # 本文件
    ├── RTSP-Player-Progress-Activity.md   # 进度跟进
    └── superpowers/
        ├── specs/
        └── plans/
```

---

## 7. Phase 实现计划（含进度）

| Phase | 内容 | 模块 | 验收标准 | 状态 | 达成率 |
|-------|------|------|----------|------|--------|
| **1** | Video 秒开链路 | RTSPlayer, StateMachine, DemuxThread, VideoDecodeThread, PacketQueue, VideoFrameQueue, AVClock(基础), SDLRenderer, PlayerStats | RTSP→SDL 窗口稳定播放、秒开、不花屏、无泄漏 | ⚠️ 已实现，首帧 ~7.3s | 70% |
| **2** | Frame Drop + A/V Sync 基础 | videoRefresh() drop 逻辑 + AVClock::isReady() + drift() | late>50ms 帧被丢弃，延迟不累积；音视频基本同步 | ⚠️ 已实现，丢帧率 46% | 60% |
| **3** | Reconnect + Serial | StreamLifecycleManager 完整 + serial 机制 | 断线自动重连，Flush 后恢复播放，旧帧自动失效 | ⚠️ 已实现，0 次重连验证 | 40% |
| **4** | Audio 链路 | AudioWorker, AudioRingBuffer, SDLAudio, AVClock::setAudioClock | 解码播放不崩溃不爆音，SDL 回调精确设时钟 | ✅ 基本稳定 | 85% |
| **5** | A/V Sync 同步模型重构 | frameTimer + compute_target_delay + drfit/lateness 解耦 + 单帧 drop | 长期播放 A/V 偏差 < 30ms，丢帧率 < 5%，skipBurst ≤ 1 | 🔧 丢帧率 0%，渲染 ~18fps（待修复 pacing） | 60% |
| **6** | 生产化打磨 | Stats CSV、窗口 resize/fullscreen、退出清理、内存泄漏检查 | 30 分钟连续播放无内存增长、无崩溃 | ⚠️ 部分完成 | 50% |

> Phase 1-6 全部基于 SDL2 + std::thread 实现，无 Qt 依赖。
> 详细进度数据、log 分析、已知问题和待办事项见 `RTSP-Player-Progress-Activity.md`。
