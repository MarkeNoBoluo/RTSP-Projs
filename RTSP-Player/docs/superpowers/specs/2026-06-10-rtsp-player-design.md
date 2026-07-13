# RTSP-Player 设计文档

## 1. 概述

**目标**: 低延迟 RTSP 拉流播放器，用于投影场景。  
**平台**: Windows, MSVC 2017 x86, C++17, FFmpeg C API (4.2.x), SDL2, CMake  
**核心诉求**: 秒开、低延迟、实时交互优先、音视频链路稳定

> **v2 架构变更 (2026-06-10)**: 移除 Qt 依赖，SDL2 替代 Qt Multimedia/OpenGL，std::thread 替代 QThread。详见 `docs/superpowers/plans/2026-06-10-sdl2-migration-plan.md`。

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
  │  VideoFrameQueue (4 slot + serial) ← VideoDecode Thread
  │
  └── SDL_Delay(1)

Demux Thread (std::thread) ── av_read_frame() + serial → PacketQueue(video) / PacketQueue(audio)
VideoDecode Thread (std::thread) ── avcodec → VideoFrameQueue (4 slot)
AudioDecode Thread (std::thread) ── avcodec → swresample → AudioRingBuffer (100ms)

SDL Audio Callback Thread (SDL 内部管理)
  └── read AudioRingBuffer → memcpy → SDL stream
  └── setAudioClock(pcm_pts + consumed_samples / sample_rate)
```

**关键约束**：
- 主循环 1ms tick 高频驱动，不依赖 SDL timer 精度
- videoRefresh() 自己判断帧显示时机，不做跨线程调度
- SDL 音频回调只做 memcpy + 设时钟，不解码（解码在独立 AudioDecode Thread）
- IRenderer 抽象接口，Phase 1 为 SDLRenderer，未来可切 D3D11Renderer
- Serial 机制保证 reconnect 后旧数据自动失效
- Windows 定时器精度：`SDL_Init` 后需调用 `timeBeginPeriod(1)` 将系统时钟粒度从默认 ~15.6ms 提升至 1ms，否则主循环 `SDL_Delay(1)` 实际阻塞 ~15ms。退出前调用 `timeEndPeriod(1)` 恢复

---

## 3. 模块清单

| 模块 | 线程 | 职责 |
|------|------|------|
| `main.cpp` | Main | SDL 窗口创建 + 高频 tick 主循环 + 事件分发 |
| `RTSPlayer` | Main | 总控（普通类，无 QObject） |
| `PlayerStateMachine` | 任意 | 6 状态机（不变） |
| `StreamLifecycleManager` | 任意 | open/reconnect/flush/close 全生命周期 + serial 管理 |
| `DemuxThread` | 独立 std::thread | `av_read_frame()` → PacketQueue (带 serial) |
| `VideoDecodeThread` | 独立 std::thread | 解码 → VideoFrameQueue (4 slot, 带 serial) |
| `AudioWorker` | 独立 std::thread | 解码 + swresample → AudioRingBuffer (100ms) |
| `PacketQueue` | 跨线程 | 环形缓冲，KeyFrame 感知 Drop，容量 200ms (video) / 80ms (audio) |
| `VideoFrameQueue` | 跨线程 | 4-Slot 环形缓冲 + serial |
| `AudioRingBuffer` | 跨线程 | 100ms 环形缓冲 + PTS chunk 追踪 + serial |
| `AVClock` | 跨线程 | VideoClock + AudioClock (pts + systemTime)，drift() |
| `SDLRenderer` | Main | IRenderer 实现：SDL_Window + SDL_Renderer + SDL_Texture |
| `SDLAudio` | Main (init) / SDL 回调 | SDL 音频设备 + 回调注册 |
| `PlayerStats` | 跨线程 | 性能统计（fps / drop / latency / queue / audio underrun） |

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

### 4.2 VideoFrameQueue（4-Slot + Serial）

四个独立 slot：decode → render → display 流水线。

```cpp
static constexpr int kSlotCount = 4;

class VideoFrameQueue {
public:
    // VideoDecodeThread 调用
    bool writeFrame(AVFrame* srcFrame, int64_t pts, int serial);

    // Main Thread (videoRefresh) 调用
    int64_t peekDisplayPts() const;        // 非阻塞，无帧返回 -1
    int     peekDisplaySerial() const;     // 检查 serial 是否匹配
    bool    advanceDisplay();              // 推进 display 指针
    bool    discardAndAdvance();           // 丢帧

    // Main Thread (paint) 调用
    AVFrame* displayFrame();               // 返回当前 display slot 的 AVFrame*

    // StreamLifecycleManager 调用
    void flush();
    void notifyAll();

private:
    AVFrame*   m_avFrames[kSlotCount];     // 永久 alloc
    VideoFrame m_slots[kSlotCount];
    int        m_serial[kSlotCount];
    int        m_renderIdx{2};
    int        m_displayIdx{0};
    int        m_decodeIdx{3};
    int        m_width{0}, m_height{0};
};
```

**Slot 流转规则**：

四个 slot 由三个指针分摊流水线阶段，余一个 slot 做缓冲，避免生产/消费撞车：

```
初始分布:  displayIdx=0  renderIdx=2  decodeIdx=3  →  空闲槽=1

  decodeIdx ──writeFrame()──▶ 写入完成 → advance: (decodeIdx + 1) % 4
  renderIdx ──等待解码完成──▶ 消费解码槽 → (renderIdx 推进到最新可用 slot)
  displayIdx ─advanceDisplay()─▶ 消费渲染槽 → displayIdx = renderIdx（renderIdx 再推进）

槽状态机 (per slot):
  FREE ──writeFrame──▶ WRITING ──写完──▶ READY
                                               │
  DISPLAYED ◀──displayFrame()── DISPLAYING ◀── advanceDisplay()
       │
       └── decodeIdx 抵达当前位置 → FREE（循环覆盖）
```

初始三个指针分散分布，确保启动后首帧即可形成 "display 有帧可显、render 有槽可产" 的流水线状态，避免第一帧触发全部指针重排的启动延迟。

**与 v1 的关键区别**：
- 3 slot → 4 slot（应对 reconnect / burst decode / render stall）
- 去掉了 `waitForNewFrame(cv)`，主循环 1ms tick 直接非阻塞 peek
- 新增 serial 字段，reconnect 后旧帧自动失效
- 去掉了 `commitDisplay()` / `discardRender()` 两步操作，简化为 `advanceDisplay()` / `discardAndAdvance()`

### 4.3 PacketQueue（KeyFrame 感知 Drop + serial）

```cpp
struct PacketNode {
    AVPacket* pkt;
    int64_t   durationUs;
    int       serial;         // v2 新增：reconnect 安全
};

class PacketQueue {
public:
    void init(AVRational timeBase, int capacityMs = 200);
    bool push(AVPacket* pkt, int serial);   // 超容量: 优先 drop P/B, 保 IDR
    bool pop(AVPacket* pkt, int timeoutMs); // 返回 false 时 pkt 未填充
    int  popSerial() const;                 // 获取队列当前 serial
    void flush();
    void abort();
};
```

### 4.4 AudioRingBuffer（替代 AudioPullDevice）

不继承 QIODevice，纯 C++ 环形缓冲。

```cpp
class AudioRingBuffer {
public:
    AudioRingBuffer(int bufferMs = 100);    // 100ms = 19,200 bytes @ 48kHz stereo s16

    // AudioWorker 解码线程调用
    bool write(const uint8_t* data, int len, double pts, int serial);
    // 满 → condition_variable::wait (替代 v1 的忙等待 QThread::msleep(1))

    // SDL 音频回调调用
    int  read(uint8_t* dst, int len, double* outPts);
    // 空 → 返回可用量 (≤ len)，调用方补静音

    void flush();
    void abort();           // 解除 write 阻塞
    int  serial() const;

private:
    struct Chunk {
        uint8_t* data;
        int32_t  len;
        double   pts;       // 起始 PTS (秒)
        int      serial;
    };
    // 环形缓冲 + mutex + cv
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
    void displayFrame(AVFrame* frame) override;   // SDL_UpdateYUVTexture + RenderCopy + Present
    void setWindowSize(int w, int h) override;
    void destroy() override;

    SDL_Window* window() const { return m_window; }
    void configureDisplayRect(int winW, int winH); // 保持宽高比的 letterbox

private:
    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture  = nullptr;
    SDL_Rect      m_dstRect;
    int           m_texW = 0, m_texH = 0;
};
```

**与 GLVideoWidget v1 的区别**：
- 无需 OpenGL shader 文件，`SDL_UpdateYUVTexture` 一行替代 65 行 GL 纹理上传
- 无需 `GL_LUMINANCE` 弃用纹理格式
- 无需 QOpenGLWidget 继承
- CPU upload（Phase 1），后续可换 D3D11Renderer GPU direct

### 4.7 SDLAudio（替代 QAudioOutput + AudioPullDevice）

```cpp
class SDLAudio {
public:
    SDLAudio(AudioRingBuffer* ringBuffer, AVClock* clock, PlayerStats* stats);
    ~SDLAudio();

    bool init(int sampleRate, int channels);   // SDL_OpenAudioDevice
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
    int m_bytesPerSample = 4;  // stereo s16 = 4 bytes

    // PTS 追踪
    double           m_currentPts = 0.0;
    int              m_chunkConsumed = 0;  // 当前 chunk 内已消费字节数
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
                           VideoFrameQueue* frameQ, AVClock* clock,
                           IRenderer* renderer);

    void setStateCallback(StateCallback cb)  { m_onState = std::move(cb); }
    void setErrorCallback(ErrorCallback cb)  { m_onError = std::move(cb); }

    bool open(const char* url);
    void close();

    IRenderer*    renderer() const { return m_renderer; }
    PlayerStats*  stats()    const { return m_stats; }
    PlayerState   state()    const;

private:
    bool initDemux(const char* url);
    bool initDecoders();
    void startThreads();
    void shutdownPipeline();

    void scheduleReconnect();   // SDL_AddTimer → SDL_USEREVENT
    void doReconnect();

    void incrementSerial() { m_pktSerial.fetch_add(1); }

    // ... members ...
    std::atomic<int>  m_pktSerial{0};  // v2 新增：serial 机制
    SDLAudio*         m_sdlAudio = nullptr;  // 替代 QAudioOutput + AudioPullDevice
    IRenderer*        m_renderer = nullptr;  // 替代 GLVideoWidget
};
```

**v2 关键变化**：
- `signals:` → `std::function<>` 回调
- `QTimer` → `SDL_AddTimer` (回调中 `SDL_PushEvent` 回主线程)
- `QAudioOutput` + `AudioPullDevice` → `SDLAudio` + `AudioRingBuffer`
- `GLVideoWidget` → `IRenderer*`
- 新增 `m_pktSerial` 管理 reconnect 安全

### 4.9 main.cpp 主循环

```cpp
int main(int argc, char* argv[]) {
    Logger::init();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
    timeBeginPeriod(1);  // 提升系统时钟粒度至 1ms（默认 ~15.6ms）

    RTSPlayer player;
    player.open(url);

    // ... event loop ...

    player.close();
    timeEndPeriod(1);
    SDL_Quit();
}
```

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

    // 渲染间距 (us)
    std::atomic<int64_t> paintIntervalMinUs{0};
    std::atomic<int64_t> paintIntervalMaxUs{0};
    std::atomic<int64_t> paintIntervalSumUs{0};
    std::atomic<int>     paintIntervalCount{0};

    // 单调帧 ID
    std::atomic<uint64_t> frameId{1};
    std::atomic<int64_t>  lastCommitUs{0};
    std::atomic<int64_t>  reconnectStartUs{0};

    // CSV
    void initCsv(const std::string& path);
    void writeCsvRow();
    void closeCsv();

    // 录制辅助 (跨线程安全)
    void recordPaintInterval(int64_t us);
    void recordPaintLatency(int64_t us);
    void recordQueueDepth(int videoMs, int audioMs);
    void recordSkipBurstEnd();

private:
    std::mutex   m_csvMutex;
    std::ofstream m_csvFile;
    std::string  m_sessionId;
    bool m_csvHeaderWritten = false;
};
```

### 4.11 Common.h

```cpp
#pragma once
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
}

enum class PlayerState : int {
    Stopped, Connecting, Playing, Recovering, Reconnecting, Error, Closing
};

struct VideoFrame {
    AVFrame* frame = nullptr;
    int64_t  pts   = AV_NOPTS_VALUE;
    int64_t  presentTime = 0;
    int      serial = 0;          // v2 新增
};

struct ClockPoint {
    double  pts;
    int64_t systemTime;
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

### A/V Sync
- 主循环 videoRefresh() 基于 AVClock::drift() 做视频帧调度
- 音频时钟由 SDL 回调精确设置（偏差 ~20ms），替代 v1 的 AudioWorker 解码时设置（偏差 0~200ms）
- video 落后 audio → 减小 delay 追赶；video 超前 → 增大 delay 等待；严重落后 → 丢帧

### Serial 机制
- StreamLifecycleManager 持有 `std::atomic<int> m_pktSerial`
- `open()` / `reconnect()` → `incrementSerial()`
- DemuxThread 每包 stamp serial，decode 线程/渲染检查匹配
- `shutdownPipeline()` → `m_pktSerial++`，所有旧数据自动失效
- 确保 RTSP reconnect 后不会显示上一连接的残留帧

### IRenderer 接口预留
- Phase 1: SDLRenderer（SDL_UpdateYUVTexture CPU upload）
- 未来: D3D11Renderer（GPU direct YUV upload）
- 接口抽象隔离渲染后端，切换不影响上层逻辑

### 秒开策略
- `avformat_open_input` 设置低 `probesize` / `analyzeduration` (~32KB)
- PacketQueue 容量仅 200ms (video) / 80ms (audio)，不强缓冲
- 首帧立即渲染

---

## 6. 目录结构

```
RTSP-Player/
├── CMakeLists.txt
├── 3rd/
│   ├── SDL2/
│   │   ├── include/          # SDL2 头文件
│   │   └── lib/x86/          # SDL2.lib + SDL2.dll
│   └── FFmpeg/
│       ├── bin/              # avcodec-58.dll 等 (x86)
│       ├── include/          # FFmpeg 头文件
│       └── lib/              # avcodec.lib 等 (x86)
├── src/
│   ├── main.cpp              # SDL 窗口 + 主循环
│   ├── RTSPlayer.h / .cpp
│   ├── StreamLifecycleManager.h / .cpp
│   ├── PlayerStateMachine.h / .cpp
│   ├── DemuxThread.h / .cpp
│   ├── VideoDecodeThread.h / .cpp
│   ├── AudioWorker.h / .cpp
│   ├── SDLRenderer.h / .cpp       # v2 新增
│   ├── SDLAudio.h / .cpp          # v2 新增
│   ├── AudioRingBuffer.h / .cpp   # v2 新增
│   ├── PacketQueue.h / .cpp
│   ├── VideoFrameQueue.h / .cpp
│   ├── AVClock.h / .cpp
│   ├── PlayerStats.h / .cpp
│   ├── Common.h
│   └── logger/
│       ├── Logger.h
│       └── Logger.cpp
└── docs/
    └── superpowers/
        ├── specs/
        │   └── 2026-06-08-rtsp-player-design.md
        └── plans/
            ├── 2026-06-08-rtsp-player-phase1.md
            └── 2026-06-10-sdl2-migration-plan.md
```

---

## 7. Phase 实现计划

| Phase | 内容 | 模块 | 验收标准 |
|-------|------|------|----------|
| **1** | Video 秒开链路 | RTSPlayer, StateMachine, DemuxThread, VideoDecodeThread, PacketQueue, VideoFrameQueue, AVClock(基础), SDLRenderer, PlayerStats | RTSP→SDL 窗口稳定播放、秒开、不花屏、无泄漏 |
| **2** | Frame Drop + A/V Sync 基础 | videoRefresh() drop 逻辑 + AVClock::isReady() + drift() | late>30ms 帧被丢弃，延迟不累积；音视频基本同步 |
| **3** | Reconnect + Serial | StreamLifecycleManager 完整 + serial 机制 | 断线自动重连，Flush 后恢复播放，旧帧自动失效 |
| **4** | Audio 链路 | AudioWorker, AudioRingBuffer, SDLAudio, AVClock::setAudioClock | 解码播放不崩溃不爆音，SDL 回调精确设时钟 |
| **5** | A/V Sync 精细调优 | AVClock::drift() 策略 + 丢帧阈值调参 + 音频 underrun 处理 | 长期播放 A/V 偏差 < 30ms |
| **6** | 生产化打磨 | Stats CSV、窗口 resize/fullscreen、退出清理、内存泄漏检查 | 30 分钟连续播放无内存增长、无崩溃 |

> Phase 1-6 全部基于 SDL2 + std::thread 实现，无 Qt 依赖。
