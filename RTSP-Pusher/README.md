# RTSP-Pusher

Windows 桌面 RTSP 直播推流工具 —— 采集屏幕画面与系统音频，H.264/AAC 编码后推送至 RTSP 服务器（如 MediaMTX）。

## 特性

- **多编码路径**：CPU 软件编码（libx264）、Intel QSV 和 NVIDIA NVENC GPU 硬件编码，按需切换
- **双架构支持**：x86（FFmpeg 4.2.x）和 x64（FFmpeg 7.x），独立构建
- **音频采集**：通过 SDL2 捕获输入设备音频，AAC 编码
- **断线重连**：指数退避重连（1s → 8s），切换 serial 自动丢弃过期数据
- **可观测性**：5 秒周期统计日志，可选 CSV 输出，涵盖码率、编码/写包延迟、队列深度、VBV 下溢、音画同步等指标
- **低延迟设计**：线程流水线分离采集/编码/推流；队列满时丢弃旧帧，避免延迟累积

## 快速开始

### 环境要求

| 依赖 | 版本 | 用途 |
|------|------|------|
| Windows | 10/11 | 运行平台 |
| MSVC | 2017 | 编译器 |
| CMake | 3.16+ | 构建系统 |
| FFmpeg | 4.2.x (x86) / 7.x (x64) | 采集、编码、推流 |
| SDL2 | 2.x | 音频采集、事件循环 |

### 构建

项目已包含 `build-x86/` 和 `build-x64/` 构建树，可直接编译：

```bash
# x86（32 位，FFmpeg 4.2.x）
cmake -B build-x86 -G "Visual Studio 15 2017" -A Win32
cmake --build build-x86 --config Release

# x64（64 位，FFmpeg 7.x，支持 GPU QSV/NVENC）
cmake -B build-x64 -G "Visual Studio 15 2017" -A x64
cmake --build build-x64 --config Release
```

编译产物分别输出至 `bin/MSVC2017_x86_Release/` 和 `bin/MSVC2017_x64_Release/`，SDL2.dll 和 FFmpeg DLL 由 CMake 构建后步骤自动复制。

### 运行

```powershell
# 默认参数启动（推送到默认 RTSP 地址，CPU 软件编码）
.\RTSP-Pusher.exe

# 指定 RTSP 地址和硬件编码器（x64 only）
.\RTSP-Pusher.exe --url rtsp://192.168.1.100:8554/live --hw-encoder qsv
.\RTSP-Pusher.exe --url rtsp://192.168.1.100:8554/live --hw-encoder nvenc

# 列出可用显示器和编码器
.\RTSP-Pusher.exe --list-screens
.\RTSP-Pusher.exe --list-encoders

# 列出音频采集设备
.\RTSP-Pusher.exe --list-audio-devices

# 自动退出（用于自动化测试）
.\RTSP-Pusher.exe --duration 30 --stats-csv stats.csv
```

按 `ESC` 或 `q` 键退出。

## 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--url <url>` | `rtsp://192.168.42.116:25544/live` | RTSP 推流地址 |
| `--list-screens` | — | 列出可用显示器并退出 |
| `--screen <n>` | `0` | 显示器：0=全部（虚拟桌面），1=主显示器，2+=指定 |
| `--capture-size WxH` | `2560x1440` | gdigrab 采集分辨率 |
| `--output-size WxH` | `1920x1080` | 编码输出分辨率 |
| `--fps <n>` | `30` | 采集/编码帧率 |
| `--bitrate <n>` | `20000` | 视频码率（kbps） |
| `--maxrate <n>` | `20000` | 最大视频码率（kbps） |
| `--bufsize <n>` | `20000` | VBV 缓冲区大小（kbits） |
| `--crf <n>` | `0` | CRF 质量；0=ABR 模式，>0=CRF 模式 |
| `--list-audio-devices` | — | 列出音频采集设备并退出 |
| `--audio-device <name>` | 默认设备 | 按名称选择 SDL 采集设备 |
| `--audio-device-index <n>` | — | 按索引选择 SDL 采集设备 |
| `--no-audio` | — | 禁用音频采集 |
| `--transport <tcp\|udp>` | `tcp` | RTSP 传输协议 |
| `--hw-encoder <name>` | `off` | 硬件编码器：`auto`、`qsv`、`nvenc`、`off` |
| `--capture-method <m>` | `auto` | 采集后端：`auto`、`gdigrab`、`ddagrab` |
| `--list-encoders` | — | 列出可用 H.264 编码器并退出 |
| `--log <path>` | `rtsp_pusher.log` | 日志文件路径 |
| `--stats-csv <path>` | — | 周期性统计 CSV 输出路径 |
| `--duration <seconds>` | — | 运行 N 秒后自动退出 |
| `--help` | — | 打印帮助信息 |

## 架构

### 线程流水线

```
ScreenCapture ──> VideoRawFrameQueue ──> VideoEncode ──> EncodedPacketQueue ──> RTSPMux ──> RTSP 服务器
                                                              ^
SDLAudioCapture ──> AudioRingBuffer ──> AudioEncode ──────────┘
```

共 5 个工作线程：

| 线程 | 核心类 | 职责 |
|------|--------|------|
| 屏幕采集 | `ScreenCaptureThread` | gdigrab → BGRA 原始帧 → VideoRawFrameQueue |
| 视频编码（CPU） | `VideoEncodeThread` | swscale 缩放 + 色彩转换 → libx264 编码 → EncodedPacketQueue |
| 视频编码（GPU） | `GpuVideoEncodeThread` | ddagrab → QSV 缩放 + h264_qsv 编码，或 ddagrab → D3D11 + h264_nvenc 编码（x64 only） |
| 音频采集 | `SDLAudioCapture` | SDL 音频回调 → AudioRingBuffer（S16 交错） |
| 音频编码 | `AudioEncodeThread` | S16 → AAC 编码 → EncodedPacketQueue |
| RTSP 封装 | `RTSPMuxThread` | 从队列取包 → av_interleaved_write_frame → RTSP 输出 |

**GPU 路径**（x64，`--hw-encoder qsv|nvenc`）：`ScreenCaptureThread` + `VideoRawFrameQueue` + `VideoEncodeThread` 被单个 `GpuVideoEncodeThread` 替代，内部使用 FFmpeg 滤镜链直接输出编码包，无中间原始帧队列。

- **QSV 后端**：`ddagrab → hwmap=qsv → scale_qsv → h264_qsv`，GPU 缩放 + 编码一体
- **NVENC 后端**：`ddagrab → D3D11 frames → h264_nvenc`，D3D11 帧直接输入编码器（当前版本 QSV 缩放不可用，编码分辨率为采集分辨率）

### 主线程与事件循环

主线程运行 SDL2 事件循环（`SDL_PollEvent` + `SDL_Delay(1)`），不执行阻塞式采集或编码。通过三个 SDL 定时器驱动异步事件：

| 定时器 | 事件码 | 周期 | 用途 |
|--------|--------|------|------|
| 重连 | `EVENT_RECONNECT` | 单次（工作线程触发） | 管线重连 |
| 统计 | `EVENT_STATS` | 5 秒 | 聚合并输出指标，写入 CSV |
| 时长 | `EVENT_DURATION` | 单次（N 秒后） | 自动化测试自动退出 |

### 状态机

```
Stopped → Opening → Streaming ⇄ Recovering
                ↓        ↓
              Error  →  Closing → Stopped
```

`PusherStateMachine` 通过原子比较交换保证状态转换合法性，非法转换被拒绝。

### 重连机制

`PusherLifecycleManager` 实现指数退避重连（1s 起，8s 上限）：

1. 任一工作线程通过 `scheduleReconnect(reason)` 上报错误，向 SDL 事件队列推送 `EVENT_RECONNECT`
2. 主线程在事件循环中处理 → `doReconnect()`
3. `doReconnect()` 调用 `stopPipeline()` + `startPipeline()`，使用新 serial
4. serial 递增后，队列中所有旧数据自动作废

### 队列

| 队列 | 类型 | 容量 | 说明 |
|------|------|------|------|
| VideoRawFrameQueue | 原始 `AVFrame*` 环形缓冲 | 固定大小 | 满时阻塞生产者 |
| AudioRingBuffer | 字节级环形缓冲 | 65536 字节（~682ms @ 48kHz 立体声 S16） | — |
| EncodedPacketQueue | 编码 `AVPacket*` 有界队列 | 60 | — |

每个队列和工作线程均携带 `serial` 号，重连后旧数据自动丢弃。

### 显示器枚举

`enumerateMonitors()` 使用 `EnumDisplayMonitors` + `GetMonitorInfo`（逻辑坐标）和 `EnumDisplaySettings`（DEVMODE 物理分辨率）获取 DPI 感知的显示器列表。显示器按主显示器优先、然后从左到右、从上到下排序。选择具体显示器时自动设置采集偏移。进程通过 `SetProcessDPIAware()` 声明 DPI 感知，确保 gdigrab 采集原生像素。

## FFmpeg 版本差异

| | x86 | x64 |
|---|-----|-----|
| FFmpeg 版本 | 4.2.x | 7.x |
| avcodec | avcodec-58 | avcodec-61 |
| avdevice | avdevice-58 | avdevice-61 |
| avformat | avformat-58 | avformat-61 |
| avfilter | avfilter-7 | avfilter-10 |
| avutil | avutil-56 | avutil-59 |
| swresample | swresample-3 | swresample-5 |
| swscale | swscale-5 | swscale-8 |

关键 API 差异：

- **x86**（FFmpeg 4.2）：需手动调用 `avcodec_register_all()`，通过 `avcodec_find_encoder_by_name()` 获取编码器，使用 `channel_layout` 字段
- **x64**（FFmpeg 7.x）：编码器自动注册，声道布局改用 `AVChannelLayout` 结构体，部分字段更名/移位

GPU 路径（QSV/NVENC）仅限 x64（`#if defined(_WIN64)` 守卫），因依赖 FFmpeg 7.x 的 `ddagrab` 采集源和对应滤镜支持。

## 统计指标

每 5 秒输出一行 `statsWindowSec=5` 日志，包含：

- 视频/音频包计数、窗口码率
- 编码延迟最大值、RTSP 写包延迟最大值（5s 窗口）
- 原始帧队列深度最大值、编码队列深度最大值、当前队列深度
- VBV 下溢次数（从 x264 日志消息中检测）
- 音频环形缓冲字节数、上溢/欠载计数
- 音画同步偏移（`audioPtsMs - videoPtsMs`）、PTS 异常计数
- 首次采集时间戳

## 项目结构

```
├── CMakeLists.txt              # 构建配置，按架构区分 DLL 版本
├── src/
│   ├── main.cpp                # 入口，CLI 解析，SDL 事件循环，显示器枚举
│   ├── RTSPusher.cpp/h         # 管线编排：startPipeline / stopPipeline
│   ├── PusherConfig.h          # 配置结构体与默认值
│   ├── PusherStateMachine.cpp/h # 原子状态机
│   ├── PusherLifecycleManager.cpp/h # 重连与 serial 管理
│   ├── ScreenCaptureThread.cpp/h    # gdigrab 采集线程
│   ├── VideoRawFrameQueue.cpp/h     # 原始帧环形缓冲
│   ├── VideoEncodeThread.cpp/h      # libx264 CPU 编码线程
│   ├── GpuVideoEncodeThread.cpp/h   # QSV/NVENC GPU 编码线程（x64 only）
│   ├── EncodedPacketQueue.cpp/h     # 编码包有界队列
│   ├── RTSPMuxThread.cpp/h          # RTSP 封装线程
│   ├── AudioRingBuffer.cpp/h        # 音频环形缓冲
│   ├── SDLAudioCapture.cpp/h        # SDL2 音频采集
│   ├── AudioEncodeThread.cpp/h      # AAC 编码线程
│   ├── PusherStats.cpp/h            # 跨线程原子统计
│   ├── PtsUtils.h                   # 系统时钟 → PTS 转换（90kHz 时基，单调保证）
│   ├── HardwareEncoderDetector.cpp/h # H.264 编码器枚举
│   ├── Common.h                     # PusherState、UserEventCode 枚举
│   └── logger/
│       └── Logger.cpp/h             # 文件日志
├── 3rd/
│   ├── FFmpeg/x86/              # FFmpeg 4.2.x（x86）
│   ├── FFmpeg/x64/              # FFmpeg 7.x（x64）
│   └── SDL2/                    # SDL2
├── build-x86/                   # x86 构建树
├── build-x64/                   # x64 构建树
├── bin/                         # 编译产物及运行时 DLL
└── docs/                        # 设计文档
```
