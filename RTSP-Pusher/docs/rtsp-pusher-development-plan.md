# RTSP-Pusher 开发计划

## 1. 计划目标

本文档用于指导 RTSP-Pusher 的分阶段开发。每个阶段只完成一组明确目标，并给出可验证的校验点，避免在功能未闭环前扩展过多模块。

参考设计文档：`docs/rtsp-pusher-design.md`。

核心交付目标：
- 通过 FFmpeg `gdigrab` 采集 Windows 桌面。
- 将 2560x1440 / 30fps 桌面画面缩放为 1920x1080 / YUV420P。
- 使用 libx264 按低延迟参数编码 H.264。
- 通过 RTSP/TCP 推送到流媒体服务器。
- 使用 SDL2 捕获音频设备 PCM，并编码后随 RTSP 推流。
- 支持基础统计、日志、异常停止和重连恢复。

---

## 2. 阶段总览

| 阶段 | 名称 | 主要目标 | 退出标准 |
|------|------|----------|----------|
| Phase 0 | 环境确认 | 确认依赖、编码器、构建链可用 | FFmpeg/SDL2/CMake/MSVC 路径和能力明确 |
| Phase 1 | 工程骨架 | 程序可编译启动，参数和日志可用 | 可运行空壳程序并输出配置 |
| Phase 2 | 屏幕采集 | gdigrab 采集桌面帧 | 采集 fps 接近 30，帧尺寸正确 |
| Phase 3 | 视频编码 | BGRA 转 YUV420P 并编码 H.264 | 可产出符合参数的 H.264 packet |
| Phase 4 | RTSP 视频推流 | 将 H.264 写入 RTSP 输出 | RTSP-Player 或 ffplay 可看到画面 |
| Phase 5 | 音频链路 | SDL 采集音频并编码 | RTSP 流包含可播放音频 |
| Phase 6 | 生命周期恢复 | 停止、清理、重连、serial 生效 | 服务中断后可恢复推流 |
| Phase 7 | 生产化打磨 | 参数化、统计、长稳测试 | 长时间推流稳定，问题可追踪 |

---

## 3. Phase 0：环境确认

### 目标

确认 RTSP-Pusher 当前依赖足以支撑后续开发，避免写完代码后才发现 FFmpeg 构建缺少关键能力。

### 实现项

- 检查 `3rd/FFmpeg/include`、`3rd/FFmpeg/lib`、`3rd/FFmpeg/bin` 是否完整。
- 检查 `3rd/SDL2/include`、`3rd/SDL2/lib/x86` 是否完整。
- 运行 `ffmpeg.exe -devices`，确认包含 `gdigrab`。
- 运行 `ffmpeg.exe -encoders`，确认包含 `libx264` 和 AAC 编码器。
- 运行 `ffmpeg.exe -formats`，确认支持 RTSP muxer。
- 运行 `ffmpeg.exe -list_devices true -f dshow -i dummy` 或 SDL 枚举逻辑，确认机器上可用音频采集设备。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| FFmpeg DLL 可用 | 执行 `3rd/FFmpeg/bin/ffmpeg.exe -version` | 命令正常输出版本 |
| gdigrab 可用 | 执行 `ffmpeg.exe -devices` | 输出中存在 `gdigrab` |
| libx264 可用 | 执行 `ffmpeg.exe -encoders` | 输出中存在 `libx264` |
| RTSP 输出可用 | 执行 `ffmpeg.exe -formats` | 输出中存在 RTSP mux/demux 支持 |
| SDL2 DLL 可用 | 检查 `3rd/SDL2/lib/x86/SDL2.dll` | 文件存在，后续程序能加载 |
| 音频设备可枚举 | 程序或命令枚举采集设备 | 至少存在一个可用输入设备，或明确记录无设备 |

### 产物

- 一份环境检查记录，可写入 `docs/validation-report-YYYY-MM-DD.md`。
- 若缺少能力，先停止后续开发，补齐依赖后再进入 Phase 1。

---

## 4. Phase 1：工程骨架

### 目标

建立最小可运行程序，完成 CMake、入口、日志、参数解析和基础对象结构。

### 实现项

- 新增 `CMakeLists.txt`，参考 RTSP-Player 链接 FFmpeg、SDL2、winmm。
- 新增 `src/main.cpp`，完成 SDL 初始化、FFmpeg log callback、参数解析和退出清理。
- 新增 `PusherConfig`，保存默认推流参数。
- 新增 `logger/Logger`，复用 RTSP-Player 的日志风格。
- 新增 `RTSPusher` 空壳类，提供 `open()`、`close()`、`state()`。
- 新增 `PusherStateMachine`，实现原子状态切换。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| CMake 配置成功 | `cmake -S . -B build` | 无配置错误 |
| 编译成功 | `cmake --build build --config Release` | 生成 exe |
| 程序可启动 | 运行 exe | 输出版本、配置和退出日志 |
| 参数可覆盖 | 传入 `--url`、`--fps`、`--size` | 日志显示覆盖后的参数 |
| 退出清理正常 | 运行后立即退出 | 无崩溃，无 SDL 未释放错误 |

### 通过标准

程序暂不采集、不编码、不推流，但可以稳定启动和退出，日志中能看到完整配置。

---

## 5. Phase 2：屏幕采集链路

### 目标

使用 FFmpeg `gdigrab` 采集桌面帧，并通过原始帧队列交给后续模块。

### 实现项

- 新增 `ScreenCaptureThread`。
- 新增 `VideoRawFrameQueue`。
- 在 `ScreenCaptureThread::openInput()` 中使用 `av_find_input_format("gdigrab")`。
- 设置 `framerate=30`、`video_size=2560x1440`、`draw_mouse=1`。
- 采集线程调用 `av_read_frame()`，记录采集帧数、错误数、队列深度。
- 队列满时丢弃最旧帧，保证低延迟。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| gdigrab 打开成功 | 运行程序启动采集 | 日志显示 input opened |
| 帧尺寸正确 | 打印首帧 width/height/pix_fmt | 尺寸为 2560x1440，格式符合 gdigrab 输出 |
| 采集帧率稳定 | 运行 60 秒统计 fps | 平均 fps 接近 30，明显低于 30 时有日志说明 |
| 队列策略生效 | 人为暂停消费或设置小队列 | 队列满后丢旧帧，不无限增长 |
| 停止安全 | 启动后关闭程序 | 采集线程 join 成功，无崩溃 |

### 通过标准

连续采集 5 分钟不崩溃，日志能看到稳定 fps、队列深度和丢帧计数。

---

## 6. Phase 3：视频编码链路

### 目标

将采集到的 BGRA 桌面帧缩放为 1920x1080 / YUV420P，并使用 libx264 编码。

### 实现项

- 新增 `VideoEncodeThread`。
- 使用 `sws_getContext()` 完成 2560x1440 BGRA 到 1920x1080 YUV420P 的转换。
- 使用 `avcodec_find_encoder_by_name("libx264")` 打开编码器。
- 设置低延迟编码参数：
  - `preset=ultrafast`
  - `tune=zerolatency`
  - `profile=main`
  - `level=5.0`
  - `crf=23`
  - `x264-params=force-cfr=1:sliced-threads=1`
  - `gop_size=30`
  - `max_b_frames=0`
  - `rc_max_rate=4000000`
  - `rc_buffer_size=7000000`
- 新增 `EncodedPacketQueue`，保存编码后 video packet。
- 视频 PTS 使用固定 `frameIndex`，time_base 为 `1/30`。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| libx264 打开成功 | 启动编码线程 | 日志显示 encoder opened |
| 像素转换成功 | 统计 sws_scale 返回值 | 返回输出高度，无转换错误 |
| packet 正常输出 | 运行 60 秒 | `videoFramesEncoded` 持续增长 |
| GOP 正确 | 记录关键帧间隔 | 约每 30 帧一个关键帧 |
| PTS 单调递增 | 检查输出 packet pts/dts | 不倒退、不出现无效时间戳 |
| 编码耗时可观测 | 记录 `encodeVideoMaxUs` | 日志或统计中可见最大耗时 |

### 通过标准

程序不推 RTSP 也能稳定完成“采集 → 转换 → H.264 packet 输出”，连续运行 5 分钟无崩溃，编码 fps 接近采集 fps。

---

## 7. Phase 4：RTSP 视频推流链路

### 目标

创建 RTSP 输出上下文，将 H.264 packet 推送到流媒体服务器，实现纯视频推流闭环。

### 实现项

- 新增 `RTSPMuxThread`。
- 使用 `avformat_alloc_output_context2(..., "rtsp", url)` 创建输出。
- 创建 video stream，将编码器参数复制到 stream。
- 设置输出参数：
  - `rtsp_transport=tcp`
  - `muxdelay=0`
- 调用 `avformat_write_header()` 建立 RTSP 会话。
- Mux 线程从 `EncodedPacketQueue` 读取 video packet。
- 写包前执行 `av_packet_rescale_ts()`。
- 调用 `av_interleaved_write_frame()` 写入 RTSP。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| RTSP header 写入成功 | 启动推流 | `avformat_write_header` 返回 0 |
| packet 写入成功 | 运行推流 | `packetsWritten` 持续增长 |
| 播放器可拉流 | 使用 RTSP-Player 或 ffplay 访问 URL | 能看到桌面画面 |
| 画面参数正确 | ffprobe 或播放器日志 | 1920x1080、H.264、约 30fps |
| 延迟未明显累积 | 观察鼠标或窗口动作 | 动作延迟保持稳定，不随时间增长 |
| 网络失败可检测 | 停止 RTSP server 或断开网络 | mux 写包报错并进入错误流程 |

### 通过标准

纯视频 RTSP 推流连续运行 10 分钟，播放器可稳定观看，队列深度不持续增长。

---

## 8. Phase 5：音频采集与编码

### 目标

使用 SDL2 捕获音频设备 PCM，编码为 AAC，并与视频一起写入 RTSP。

### 实现项

- 新增 `SDLAudioCapture`。
- 新增或复用 `AudioRingBuffer`。
- 新增 `AudioEncodeThread`。
- 枚举 SDL capture device，支持指定设备名。
- 使用 `SDL_OpenAudioDevice(deviceName, 1, ...)` 打开采集设备。
- SDL 回调只写 PCM 到 ring buffer，不做重采样和编码。
- 使用 `swresample` 将 SDL 输入格式转换为 AAC 编码器需要的格式。
- 使用 AAC 编码器输出 audio packet。
- Mux 线程同时处理 video/audio packet。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| 音频设备可打开 | 启动程序并启用音频 | 日志显示 capture device opened |
| PCM 数据进入 ring | 统计 `audioBytesCaptured` | 数值持续增长 |
| AAC 编码成功 | 统计 `audioFramesEncoded` | 数值持续增长 |
| RTSP 包含音频流 | ffprobe 检查 RTSP URL | 存在 audio stream |
| 播放器可听到声音 | RTSP-Player 或 ffplay 播放 | 可听到采集设备声音 |
| 音频 PTS 连续 | 检查 packet pts | 单调递增，无大段断裂 |
| 无音频时行为明确 | 拔掉或禁用设备 | 日志明确报错，程序不假装成功 |

### 通过标准

音视频 RTSP 推流连续运行 10 分钟，播放器可同时看到画面和听到声音。若当前机器没有可用采集设备，应明确记录该限制，不把音频阶段标为通过。

---

## 9. Phase 6：生命周期恢复

### 目标

完善启动、停止、异常恢复和重连流程，避免线程泄漏、旧数据污染新连接。

### 实现项

- 完成 `PusherLifecycleManager`。
- 所有线程支持 `start()`、`stop()`、`join()`。
- 所有队列支持 `flush()`、`abort()`、`reset(serial)`。
- 引入 serial 机制：
  - 每次 open/reconnect 递增 serial。
  - capture、encode、mux 都检查 serial。
  - 不匹配的帧或包直接丢弃。
- Mux 写包失败时触发 `scheduleReconnect()`。
- 使用 SDL user event 回到主线程执行 `doReconnect()`。
- 实现指数退避：1s、2s、4s、8s。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| 正常关闭 | 推流中退出程序 | 所有线程退出，无崩溃 |
| 重复启动停止 | 连续 open/close 10 次 | 无死锁、无资源泄漏迹象 |
| RTSP server 重启恢复 | 推流中重启服务端 | 程序进入重连并恢复推流 |
| 旧包失效 | 重连时打印 serial | 旧 serial packet 不写入新连接 |
| 队列清理 | 重连前后记录队列深度 | reconnect 后队列从空开始 |
| 状态机正确 | 记录状态迁移 | 无非法跳转或卡死状态 |

### 通过标准

模拟 RTSP 服务中断和恢复 3 次，程序都能自动恢复推流；退出时所有线程和 FFmpeg/SDL 资源被释放。

---

## 10. Phase 7：生产化打磨

### 目标

补齐实际使用需要的参数、统计、日志和长时间稳定性验证。

### 实现项

- 完善命令行参数：
  - `--url`
  - `--capture-size`
  - `--output-size`
  - `--fps`
  - `--bitrate`
  - `--maxrate`
  - `--bufsize`
  - `--crf`
  - `--audio-device`
  - `--no-audio`
  - `--transport`
  - `--log`
  - `--stats-csv`
- 新增 `PusherStats` CSV 输出。
- 每 5 秒输出一次统计快照。
- 增加 FFmpeg 错误码转字符串。
- 增加首帧、首包、首个 RTSP 写包耗时统计。
- 增加长时间运行测试记录。

### 校验点

| 校验点 | 方法 | 通过标准 |
|--------|------|----------|
| 参数覆盖生效 | 使用不同命令行参数启动 | 日志显示实际生效参数 |
| CSV 可写 | 指定 `--stats-csv` | 文件生成且字段完整 |
| 码率可观测 | 运行推流 | 统计中有实时 kbps |
| 长稳测试 | 连续推流 30 分钟 | 无崩溃，队列不持续增长 |
| CPU 占用记录 | 观察任务管理器或日志 | CPU 占用在可接受范围内并记录 |
| 错误可定位 | 人为制造设备或网络错误 | 日志包含模块、错误码、错误文本 |

### 通过标准

程序具备实际运行可用性：可参数化启动，可观测运行状态，连续 30 分钟推流无崩溃，异常场景能定位原因。

---

## 11. 最终验收清单

| 类别 | 验收项 | 标准 |
|------|--------|------|
| 构建 | Release 可编译 | MSVC 2017 x86 构建通过 |
| 视频 | 桌面采集 | 2560x1440 / 30fps gdigrab 输入正常 |
| 视频 | 编码输出 | H.264 main profile，1920x1080，YUV420P |
| 视频 | 关键帧 | GOP 约 30，1 秒一个关键帧 |
| 推流 | RTSP/TCP | 目标服务器可接收 |
| 推流 | 播放验证 | RTSP-Player 或 ffplay 可播放 |
| 音频 | SDL 采集 | 可选择或使用默认采集设备 |
| 音频 | AAC 编码 | RTSP 中存在可播放音频流 |
| 同步 | 时间戳 | 音视频 PTS 单调递增，无明显断裂 |
| 稳定性 | 退出 | 无崩溃，无线程悬挂 |
| 稳定性 | 重连 | 服务中断后自动恢复 |
| 可观测 | 日志 | 关键状态、错误、参数均有记录 |
| 可观测 | 统计 | fps、码率、队列、错误计数可见 |

---

## 12. 开发顺序要求

- 未完成 Phase 0，不进入编码实现。
- 未完成 Phase 2，不实现视频编码。
- 未完成 Phase 3，不接 RTSP 输出。
- 未完成 Phase 4，不接音频链路。
- 未完成 Phase 5，不做大规模参数化和打磨。
- 每个阶段完成后先跑对应校验点，再进入下一阶段。

这样安排是为了先形成“视频采集 → 视频编码 → RTSP 视频推流”的最小闭环，再补音频和恢复能力。
