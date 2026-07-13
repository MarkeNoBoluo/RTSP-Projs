# RTSP-Proj 项目逻辑梳理

## 一、项目定位

本目录由三个并列子目录组成：

| 目录 | 当前内容 | 定位 |
|---|---|---|
| `RTSP-Player/` | 完整 CMake/C++ 工程 | 从 RTSP 服务端拉流，完成解复用、音视频解码、SDL 音频播放和视频渲染 |
| `RTSP-Pusher/` | 完整 CMake/C++ 工程 | 采集 Windows 桌面与 SDL 音频，编码为 H.264/AAC，并推送到 RTSP 服务端 |
| `RTSP-Server/` | 仅有两行 README | 服务端占位说明，不包含服务器实现 |

播放器与推流器彼此独立构建和运行，通过外部 RTSP 服务端间接通信；仓库内没有将二者链接为同一可执行程序，也没有实现 RTSP Server。

## 二、技术栈与构建边界

- 语言标准：C++17。
- 平台与编译器：Windows、MSVC 2017。
- 构建系统：CMake 3.16 及以上。
- 媒体能力：FFmpeg C API。
- 窗口、事件和音频：SDL2。
- 架构：x86 与 x64，CMake 按指针宽度自动选择依赖与输出目录。
- FFmpeg 版本：
  - x86 使用 FFmpeg 4.2.x。
  - x64 使用 avcodec 61、avformat 61、avutil 59 等新版库。
- 构建结果输出到各子工程的 `bin/MSVC2017_<ARCH>_<CONFIG>/`，构建后自动复制 SDL2 和 FFmpeg DLL。

两个工程都是单进程、多线程、无 Qt 的 SDL2 应用。`RTSP-Player/src/test_qt.cpp` 是未被 CMake 纳入编译的 v1 遗留文件。

## 三、RTSP-Player

### 3.1 总体数据流

```text
RTSP Server
  -> FFmpeg av_read_frame
  -> DemuxThread
     -> video PacketQueue -> VideoDecodeThread -> VideoFrameQueue
        -> 主线程 RTSPlayer::videoRefresh -> SDLRenderer -> SDL 窗口
     -> audio PacketQueue -> AudioWorker -> AudioRingBuffer
        -> SDL 音频回调 SDLAudio -> 音频设备
```

主线程负责 SDL 事件循环、视频刷新、定时统计、重连事件和退出；阻塞式网络读取与解码都位于工作线程。

### 3.2 核心组件

| 组件 | 文件 | 职责 |
|---|---|---|
| 程序入口 | `RTSP-Player/src/main.cpp` | CLI 解析、日志与 SDL 初始化、事件循环、统计定时器、退出清理 |
| 播放器外观 | `RTSP-Player/src/RTSPlayer.*` | 对外提供 open/close/config，执行视频时序、丢帧与音画同步 |
| 生命周期管理 | `RTSP-Player/src/StreamLifecycleManager.*` | 创建和销毁 FFmpeg 上下文、队列与线程，处理硬解、低延迟和重连 |
| 解复用线程 | `RTSP-Player/src/DemuxThread.*` | `av_read_frame`，按 stream index 分发音视频包，检测超时和流错误 |
| 视频解码线程 | `RTSP-Player/src/VideoDecodeThread.*` | FFmpeg 视频解码、DXVA2 帧回传、PTS 换算、写入帧队列 |
| 音频工作线程 | `RTSP-Player/src/AudioWorker.*` | 音频解码、重采样为 48 kHz 双声道 S16、写入音频环形缓冲 |
| 视频渲染 | `RTSP-Player/src/SDLRenderer.*` | 非 NV12 帧经 swscale 转 NV12，更新 SDL 纹理并按比例渲染 |
| 音频输出 | `RTSP-Player/src/SDLAudio.*` | SDL 播放回调读取 PCM，不足时补静音，并推进音频时钟 |
| 包队列 | `RTSP-Player/src/PacketQueue.*` | 按媒体时长限制容量；视频默认等待腾空，低延迟音频可丢旧包 |
| 视频帧队列 | `RTSP-Player/src/VideoFrameQueue.*` | 24 槽环形队列；满时覆盖最旧待显示帧 |
| 音频缓冲 | `RTSP-Player/src/AudioRingBuffer.*` | 按带 PTS 的 chunk 保存 PCM，支持部分读取和精确播放时钟 |
| 时钟 | `RTSP-Player/src/AVClock.*` | 保存音频、视频 PTS 与系统时间锚点，计算漂移 |
| 状态与统计 | `PlayerStateMachine.*`、`PlayerStats.*` | 原子状态机、运行指标与 CSV 输出 |

### 3.3 启动链路

```text
main
  -> 解析 URL、transport、音频、低延迟、硬解等参数
  -> SDL_Init(VIDEO | AUDIO | TIMER)
  -> 创建 SDLRenderer 和 RTSPlayer
  -> RTSPlayer::open
  -> StreamLifecycleManager::open
     -> initDemux
        -> avformat_open_input
        -> avformat_find_stream_info
        -> 定位视频与可选音频流
        -> 初始化音视频 PacketQueue
     -> initDecoders
        -> 视频软件解码或 DXVA2
        -> 可选音频解码
     -> startThreads
        -> serial 加一
        -> 先启动视频解码、音频解码和 SDL 音频
        -> 等待消费者 ready
        -> 最后启动 DemuxThread
```

视频是必需流；找不到视频会打开失败。音频可缺失，也可由 `--no-audio` 主动禁用。SDL 音频设备打开失败时会降级为无音频播放。

### 3.4 播放与音画同步

`RTSPlayer::videoRefresh()` 由主线程约每 1 ms 调用：

1. 丢弃 serial 与当前流水线不一致的旧帧。
2. 无音频的 `--setpts-zero` 模式只保留最新帧并立即渲染。
3. 首帧在视频时钟尚未建立时立即渲染。
4. 有音频时以 SDL 音频回调推进的音频时钟为基准，比较当前帧 PTS 与音频时钟。
5. 视频明显落后时，先从帧队列连续丢帧；仅剩一帧仍落后时进入快速追赶。
6. 视频超前时延长等待；到达目标时间后才渲染。
7. 单帧过期超过 50 ms 且满足最小丢帧间隔时丢弃。
8. 渲染成功后更新视频时钟、统计数据并推进帧队列。

正常模式目标总延迟约 500–800 ms；`--setpts-zero` 会缩短包队列、音频缓冲和 SDL 回调粒度，音视频模式目标约 300 ms。

### 3.5 断流与关闭

`DemuxThread` 的 FFmpeg interrupt callback 在主动停止或连续读取超过 3 秒时中断读取。读取错误触发：

```text
Playing -> Recovering
  -> shutdownPipeline
  -> Reconnecting
  -> SDL 单次定时器等待 1/2/4 秒
  -> 主线程 doReconnect
  -> 重新建立 demux、decoder 和线程
```

当前最多重连 3 次，因此虽然单次退避计算本身封顶 8 秒，实际在达到 8 秒前已经停止尝试。成功后重置退避计数并累计恢复耗时。关闭顺序先 abort 队列与停止音频，再 join 工作线程，最后释放 FFmpeg 上下文、清空队列和复位时钟。

## 四、RTSP-Pusher

### 4.1 总体数据流

CPU 路径：

```text
FFmpeg gdigrab -> ScreenCaptureThread -> VideoRawFrameQueue(4)
  -> VideoEncodeThread(libx264/QSV/NVENC) -> EncodedPacketQueue(60)
SDL capture callback -> AudioRingBuffer(65536 bytes)
  -> AudioEncodeThread(AAC) --------------------^
EncodedPacketQueue -> RTSPMuxThread -> FFmpeg RTSP muxer -> RTSP Server
```

x64 GPU 路径：

```text
GpuVideoEncodeThread:
  ddagrab -> D3D11 frames -> QSV filter/encoder 或 NVENC encoder
  -> EncodedPacketQueue -> RTSPMuxThread
```

GPU 路径会替代 CPU 路径中的 `ScreenCaptureThread + VideoRawFrameQueue + VideoEncodeThread`。

### 4.2 核心组件

| 组件 | 文件 | 职责 |
|---|---|---|
| 程序入口 | `RTSP-Pusher/src/main.cpp` | CLI、DPI 感知、显示器/音频设备枚举、SDL 事件循环、统计与退出 |
| 推流器编排 | `RTSP-Pusher/src/RTSPusher.*` | 按配置创建 CPU/GPU 视频、音频、队列和 mux 流水线 |
| 生命周期管理 | `RTSP-Pusher/src/PusherLifecycleManager.*` | 跨线程错误汇聚、serial 和指数退避重连 |
| 桌面采集 | `RTSP-Pusher/src/ScreenCaptureThread.*` | FFmpeg gdigrab 输入、解码 BGRA 原始帧并写入原始帧队列 |
| CPU 视频编码 | `RTSP-Pusher/src/VideoEncodeThread.*` | 缩放/像素格式转换、H.264 编码、生成墙钟驱动的单调 PTS |
| GPU 视频编码 | `RTSP-Pusher/src/GpuVideoEncodeThread.*` | x64 ddagrab、D3D11/QSV 过滤链及硬件 H.264 编码 |
| 音频采集 | `RTSP-Pusher/src/SDLAudioCapture.*` | SDL 采集回调把设备 PCM 写入环形缓冲 |
| 音频编码 | `RTSP-Pusher/src/AudioEncodeThread.*` | 按实际 SDL 格式重采样并 AAC 编码 |
| RTSP 发送 | `RTSP-Pusher/src/RTSPMuxThread.*` | 创建 RTSP 输出流、重缩放时间戳、交错写包、检测写错误 |
| 原始帧队列 | `RTSP-Pusher/src/VideoRawFrameQueue.*` | 容量 4，满时丢弃最旧帧，防止采集延迟累积 |
| 编码包队列 | `RTSP-Pusher/src/EncodedPacketQueue.*` | 容量 60；溢出后清队列并等待下一个视频关键帧恢复 |
| 配置、状态、统计 | `PusherConfig.h`、`PusherStateMachine.*`、`PusherStats.*` | 默认参数、原子状态、性能与音画指标 |

### 4.3 启动链路

```text
main
  -> 设置 DPI aware，解析参数并解析显示器坐标
  -> 根据 hw-encoder/capture-method 选择 gdigrab 或 ddagrab
  -> SDL_Init(AUDIO | TIMER | EVENTS)
  -> RTSPusher::open
     -> startPipeline，生成新 serial
     -> 创建编码包队列
     -> 启动 CPU 或 GPU 视频编码器
     -> 可选：打开 SDL 音频设备并启动 AAC 编码器
     -> 创建 RTSPMuxThread，写 RTSP header 并启动发送线程
     -> 启动音频采集
     -> CPU 路径最后启动屏幕采集
  -> Streaming
```

音频默认启用。默认设备失败时允许继续纯视频推流；用户显式指定音频设备后，`requireAudio=true`，音频初始化失败会令整个打开过程失败。

### 4.4 视频编码与 PTS

- CPU 路径把 gdigrab 解码得到的 BGRA 帧缩放并转为 YUV420P/NV12。
- H.264 编码使用 90 kHz time base、无 B 帧、低延迟和 global header。
- libx264 使用 `veryfast + zerolatency`，并关闭场景切换关键帧。
- `wallClockToVideoPts()` 以该 serial 的首帧采集时间为基准，并保证 PTS 严格单调。
- 音频 PTS 按累计样本数生成。
- mux 前通过 `av_packet_rescale_ts` 从编码器 time base 转换到输出流 time base。

### 4.5 重连与关闭

编码器或 mux 工作线程调用 `scheduleReconnect()`，该函数用原子标志合并重复请求并向 SDL 主线程投递 `EVENT_RECONNECT`。主线程执行：

```text
stopPipeline
  -> serial 加一
  -> 主线程同步等待 1/2/4/8 秒
  -> startPipeline
```

推流器的停止顺序是采集、音频采集与编码、视频编码、RTSP mux、队列，保证先停止生产者、最后停止写出者。

当前实现中，`doReconnect()` 在重启前调用一次 `nextSerial()`，`startPipeline()` 又调用一次，因此每轮重连 serial 实际增加 2。生命周期代码也没有把状态机切换到 `Recovering/Reconnecting`，重连期间对外状态通常仍保持 `Streaming`。

## 五、两端关系与协议边界

```text
RTSP-Pusher --RTSP/RTP--> 外部 RTSP Server --RTSP/RTP--> RTSP-Player
```

- 推流器不直接调用播放器。
- 播放器不直接控制推流器。
- 两端共享的是媒体协议约定：H.264 视频、可选 AAC 音频、RTSP transport 为 TCP 或 UDP。
- 当前 `RTSP-Server/` 没有可运行实现；实际部署需要 MediaMTX 等外部 RTSP Server。

## 六、关键状态与一致性机制

播放器状态：

```text
Stopped -> Connecting -> Playing -> Recovering -> Reconnecting
                  \-> Error                         |
Closing -> Stopped <--------------------------------/
```

推流器状态定义为：

```text
Stopped -> Opening -> Streaming -> Recovering -> Reconnecting
               \-> Error
Closing -> Stopped
```

两个工程都用原子状态机保存状态，并用 serial/generation 标记流水线代次。重连后新线程、包和帧使用新 serial，消费者丢弃旧 serial 数据，避免旧连接残留数据污染新连接。

## 七、运行参数重点

播放器主要参数：`--url`、`--transport tcp|udp`、`--no-audio`、`--setpts-zero`、`--hwaccel auto|dxva2|none`、`--fullscreen`、`--csv`、`--exit-after`。

推流器主要参数：`--url`、`--screen`、`--capture-size`、`--output-size`、`--fps`、`--bitrate`、`--maxrate`、`--bufsize`、`--crf`、`--audio-device`、`--no-audio`、`--transport`、`--hw-encoder`、`--capture-method`、`--stats-csv`、`--duration`。

## 八、当前仓库观察

- 两个子工程都保留了较完整的设计、验证和指标说明文档。
- `RTSP-Pusher` 工作树当前已有用户修改和构建产物变化；本次梳理未改动这些文件。
- `RTSP-Pusher` 的说明文档将重连描述为 SDL 定时器驱动，但当前源码实际是 SDL 事件触发后，由主线程在 `doReconnect()` 内同步 `av_usleep()` 完成退避。
- `RTSP-Pusher` 已定义 `Recovering/Reconnecting` 状态，但当前重连实现没有进入这两个状态，并且每轮重连会递增两次 serial。
- `PusherConfig.h` 当前 `videoMaxrate` 默认值为 8000 kbps，而 `main.cpp` 帮助文本和 README 写的是 20000 kbps；运行时未显式传参时以代码中的 8000 为准。
- `RTSP-Server` 目前只是占位目录，不能单独构建或运行。
