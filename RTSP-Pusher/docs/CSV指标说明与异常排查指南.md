# CSV 指标说明与异常排查指南

## 一、概述

RTSP-Pusher 通过 `--stats-csv <path>` 参数启用 CSV 统计输出，每 5 秒写入一行，包含 28 列指标（1 列时间戳 + 27 列数据）。CSV 格式为逗号分隔，秒级刷新（`fflush`），可配合外部监控工具实时分析推流质量。

### CSV 列头

```
timestamp,videoCaptured,videoDropped,videoEncoded,audioBytes,audioEncoded,videoPackets,audioPackets,packetsWritten,writeErrors,reconnects,rawQueue,rawQMax,encQueue,encQMax,bitrateKbps,encMaxUs,muxWriteMaxUs,vbvUnderflow,audioRingBytes,audioOverflow,audioUnderrun,videoPtsMs,audioPtsMs,avOffsetMs,ptsErrorCount,firstVideoCaptureUs,firstAudioCaptureUs
```

---

## 二、指标分类详解

### 2.1 视频采集与编码

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `videoCaptured` | int64_t 累计 | 从屏幕采集的原始帧总数 | 单调递增，每 5 秒增加约 `fps × 5` |
| `videoDropped` | int64_t 累计 | 因原始帧队列满丢弃的帧数 | **0**（任何 >0 均为异常） |
| `videoEncoded` | int64_t 累计 | 成功编码输出的帧数 | 与 `videoCaptured` 基本一致，差值 ≤ 队列容量(4) |

**异常排查：**

- **`videoDropped` > 0**：编码速度跟不上采集速度。
  - 方向：降低 `--capture-size` 或 `--fps`；CPU 路径检查 `encMaxUs` 是否过高；GPU 路径检查 QSV 驱动
  - 关联指标：`rawQMax` 达到或接近 4（队列容量），`encMaxUs` > 33000（编码耗时超过帧间隔）
- **`videoEncoded` 长期小于 `videoCaptured`**：编码线程停滞。
  - 方向：检查编码器初始化日志，查看是否有编码错误输出；CPU 路径确认 libx264 DLL 加载正常

### 2.2 音频采集与编码

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `audioBytes` | int64_t 累计 | SDL 采集写入环形缓冲区的 PCM 字节总数 | 约 `sampleRate × channels × 2 × 运行秒数`（48kHz 立体声 S16 = ~192KB/s） |
| `audioEncoded` | int64_t 累计 | 成功编码的 AAC 帧数 | 约 `运行秒数 × (1000/编码帧时长ms)`，通常每帧 21.3ms |

**异常排查：**

- **`audioBytes` 不增长**：音频采集停滞。
  - 方向：检查 `--audio-device` 参数是否正确；确认系统音频设备未被其他程序独占；查日志中是否有 SDL 音频初始化错误
- **`audioEncoded` 不增长但 `audioBytes` 增长**：音频编码器故障。
  - 方向：检查 AAC 编码器初始化日志；确认 FFmpeg AAC 编码器 DLL 正常

### 2.3 RTSP 推流统计

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `videoPackets` | int64_t 累计 | 写入 RTSP 的视频 RTP 包数 | 单调递增 |
| `audioPackets` | int64_t 累计 | 写入 RTSP 的音频 RTP 包数 | 单调递增 |
| `packetsWritten` | int64_t 累计 | `av_interleaved_write_frame` 成功写入的总包数 | 单调递增，约等于 `videoPackets + audioPackets` |
| `writeErrors` | int64_t 累计 | 写入失败次数 | **0**（任何 >0 均为异常） |
| `reconnects` | int64_t 累计 | 重连次数 | **0**（稳定网络下应为 0） |

**异常排查：**

- **`writeErrors` > 0**：RTSP 推流写入失败。
  - 方向：检查 RTSP 服务器是否可达（`--url`）；确认 `--transport tcp` 切换传输协议；查看日志中的 `av_interleaved_write_frame` 错误码（负值）
  - 网络层面：ping RTSP 服务器 IP，检查防火墙规则
- **`reconnects` > 0**：触发了重连流程。
  - 方向：查看日志中的 `scheduleReconnect` 原因；检查是否由 `writeErrors` 触发；确认重连后的 serial 是否正确递增
  - 频繁重连：降低 `--bitrate` 减轻网络压力；检查 RTSP 服务器负载

### 2.4 队列深度

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `rawQueue` | int | 原始视频帧队列当前大小（快照值） | 0~4 |
| `rawQMax` | int | 5 秒窗口内原始帧队列峰值 | 0~4 |
| `encQueue` | int | 编码包队列当前大小（快照值） | 0~60 |
| `encQMax` | int | 5 秒窗口内编码包队列峰值 | 0~60 |

**异常排查：**

- **`rawQMax` = 4（满）**：队列满导致 `videoDropped` 增加。
  - 方向：见 2.1 节 `videoDropped` 的排查方向
- **`encQMax` 接近 60（满）**：RTSP 写入速度跟不上编码输出。
  - 方向：检查网络带宽和 `bitrateKbps`；确认 RTSP 服务器写入吞吐量；检查 `muxWriteMaxUs` 是否过高
  - 极端情况：编码队列满会反压编码线程，导致编码帧延迟增加

### 2.5 性能指标（每 5 秒窗口）

| CSV 列名 | 类型 | 含义 | 正常区间 | 异常阈值 |
|----------|------|------|----------|----------|
| `bitrateKbps` | int | 5 秒窗口实际输出码率 | 接近 `--bitrate` 设定值 | 偏差超过 30% 需关注 |
| `encMaxUs` | int64_t | 窗口内单帧编码最大耗时（微秒） | < 33000（@30fps） | > 33000 开始丢帧 |
| `muxWriteMaxUs` | int64_t | 窗口内单次 `av_interleaved_write_frame` 最大耗时（微秒） | < 5000 | > 10000 需关注 |

**异常排查：**

- **`bitrateKbps` 远低于设定值**：编码器未产生足够数据。
  - 方向：`--crf 0` 确认处于 ABR 模式；检查 `--maxrate` 和 `--bufsize` 是否限制了码率；VBV 相关见 2.6 节
- **`encMaxUs` > 33000**：单帧编码超过帧间隔，开始丢帧。
  - 方向：降低 `--capture-size` 和 `--output-size`；降低 `--fps`（减少帧率压力）；CPU 路径考虑加减速预设（如 `--preset` 从 medium 改为 veryfast，如有此参数）；GPU 路径确认 QSV 正常工作
- **`muxWriteMaxUs` > 10000**：RTSP 写入延迟过高。
  - 方向：检查网络延迟（到 RTSP 服务器的 RTT）；尝试切换 `--transport udp`（UDP 对延迟更宽容）；确认服务器处理能力

### 2.6 VBV 码率控制

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `vbvUnderflow` | int64_t 累计 | x264 VBV buffer underflow 次数 | **0**（任何 >0 表示码率控制失衡） |

**异常排查：**

- **`vbvUnderflow` > 0**：编码器 VBV 缓冲区下溢，说明 bitrate/bufsize/maxrate 参数不匹配。
  - 方向：增大 `--bufsize`（如从 20000 增至 40000 kbits）；增大 `--maxrate`（允许更高瞬时码率）；降低 `--crf` 值或增大 `--bitrate`；降低视频复杂度（降低分辨率、帧率）
  - 原理：VBV（Video Buffering Verifier）是编码器的虚拟缓冲区模型，underflow 表示在当前参数下无法保证恒定码率输出

### 2.7 音频环形缓冲区

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `audioRingBytes` | int | 环形缓冲区当前可读字节数 | 0~65536，正常波动 |
| `audioOverflow` | int64_t 累计 | 音频缓冲区上溢（SDL 写入时无空间）次数 | **0**（任何 >0 表示音频编码跟不上采集） |
| `audioUnderrun` | int64_t 累计 | 音频缓冲区下溢（编码线程等待 >20ms）次数 | **0**（任何 >0 表示音频采集跟不上编码） |

**异常排查：**

- **`audioOverflow` > 0**：SDL 采集速度快于编码速度。
  - 方向：缓冲区满 65536 字节（~682ms @ 48kHz 立体声 S16），编码线程可能卡在 IO 等待；检查编码队列是否满（`encQMax` 接近 60）
- **`audioUnderrun` > 0**：编码线程得不到足够 PCM 数据。
  - 方向：检查 SDL 音频设备是否正常回调；是否开启了 `--no-audio`；确认音频设备采样率匹配
- **`audioRingBytes` 持续为 0**：音频采集已停止。
  - 方向：同 2.2 节 `audioBytes` 不增长的排查方向

### 2.8 音视频同步

| CSV 列名 | 类型 | 含义 | 正常区间 |
|----------|------|------|----------|
| `videoPtsMs` | int64_t | 最新视频 PTS（媒体时间，毫秒） | 单调递增，与运行时长基本一致 |
| `audioPtsMs` | int64_t | 最新音频 PTS（媒体时间，毫秒） | 单调递增，与运行时长基本一致 |
| `avOffsetMs` | int64_t | 音画同步偏移（`audioPtsMs - videoPtsMs`） | -200 ~ +200 ms |
| `ptsErrorCount` | int64_t 累计 | PTS 非单调递增异常次数 | **0**（任何 >0 表示 PTS 回退） |

**异常排查：**

- **`avOffsetMs` 绝对值持续 > 500ms**：音画不同步。
  - `avOffsetMs` > 0（正偏移）：音频领先视频 → 视频编码或采集慢于音频
  - `avOffsetMs` < 0（负偏移）：视频领先音频 → 音频编码或采集慢于视频
  - 方向：检查 `encMaxUs` 确认编码是否延迟；确认 `videoDropped` 是否丢帧导致 PTS 跳跃；检查音频 `underrun` 是否导致音频 PTS 停滞
  - 根本原因通常与帧率/采样率配置不一致或编码性能瓶颈有关
- **`ptsErrorCount` > 0**：PTS 出现回退（非单调）。
  - 方向：检查编码线程的 PTS 计算逻辑；确认 `serialStartUs` 在重连后正确更新；查看日志中是否有时间戳异常打印

### 2.9 首帧时间戳

| CSV 列名 | 类型 | 含义 |
|----------|------|------|
| `firstVideoCaptureUs` | int64_t | 首帧视频编码时刻的墙钟时间（av_gettime_relative，微秒） |
| `firstAudioCaptureUs` | int64_t | 首帧音频编码时刻的墙钟时间（av_gettime_relative，微秒） |

这两个指标主要用于诊断音视频启动时序：两者差值反映首帧初始化先后，正常差值应在几百毫秒内。

---

## 三、综合异常场景排查矩阵

| 症状 | 关键指标 | 排查方向 |
|------|----------|----------|
| 画面卡顿 | `videoDropped` > 0, `rawQMax` = 4, `encMaxUs` > 33000 | 降低分辨率/帧率；检查编码器性能 |
| 网络带宽不足 | `writeErrors` > 0, `encQMax` 接近 60, `bitrateKbps` 大幅波动 | 降低 `--bitrate`；切换 TCP/UDP；检查网络 |
| 音画不同步 | `avOffsetMs` 绝对值 > 500, `ptsErrorCount` > 0 | 检查 encMaxUs/audioUnderrun 确定瓶颈端 |
| 频繁重连 | `reconnects` 快速增长, `writeErrors` 同步增长 | 检查 RTSP 服务器可用性；网络稳定性 |
| VBV 码率异常 | `vbvUnderflow` > 0 | 调整 bufsize/maxrate/bitrate 参数 |
| 音频丢失 | `audioUnderrun` > 0, `audioRingBytes` 持续为 0 | 检查 SDL 设备；确认音频驱动正常 |
| 音频断流 | `audioOverflow` > 0 | 降低编码队列压力；检查编码线程是否阻塞 |
| 编码卡死 | `videoEncoded` 不增长, `encMaxUs` 极高 | 检查编码器日志；确认 FFmpeg DLL 版本匹配 |
| CPU 过载 | `encMaxUs` 持续接近或超过帧间隔 | 降低分辨率/帧率/预设质量；考虑 GPU 路径 |

### 排查流程（推荐顺序）

1. **先看 `writeErrors`** — 如果有错误，网络层或 RTSP 服务器是首要问题
2. **看 `reconnects`** — 确认连接稳定性
3. **看 `videoDropped` 和 `rawQMax`** — 判断编码/采集瓶颈
4. **看 `encMaxUs`** — 确认单帧编码耗时是否异常
5. **看 `vbvUnderflow`** — 确认码率控制参数是否合理
6. **看 `avOffsetMs` 和 `ptsErrorCount`** — 确认时间戳和同步
7. **看音频指标** (`audioOverflow`, `audioUnderrun`, `audioRingBytes`) — 确认音频链路
8. **看 `encQMax`** — 确认推流输出是否跟得上

---

## 四、CSV 数据特性说明

1. **累计值与快照值**：`videoCaptured`、`videoDropped`、`videoEncoded`、`audioBytes`、`audioEncoded`、`packetsWritten`、`writeErrors`、`reconnects`、`vbvUnderflow`、`audioOverflow`、`audioUnderrun`、`ptsErrorCount` 为累计值（从启动起累加）；其余为每 5 秒快照值。
2. **窗口最大值**：`rawQMax`、`encQMax`、`encMaxUs`、`muxWriteMaxUs` 是 5 秒窗口内的峰值，非累计值。
3. **`reconnects` 不重置**：即使调用 `PusherStats::reset()`，重连计数也会保留（跨重连累积）。
4. **`encQMax` 的实现局限**：该值由多个线程各自写入，并非真正的窗口最大值跟踪，而是各线程在窗口结束时的队列大小快照，最后一次写入的值会覆盖之前的。
5. **时间戳**：CSV 第一列 `timestamp` 是 Unix 时间戳（`time(nullptr)`），精度为秒级；指标内部使用 `av_gettime_relative()` 获取微秒级时间。
