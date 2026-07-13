# V2 基线审计报告

**日期**: 2026-06-18
**测试时长**: 600 秒（10 分钟）
**二进制**: bin/MSVC2017_x86_Release/RTSP-Pusher.exe

## 1. 环境信息

- **OS**: Microsoft Windows 11 家庭版 中文版
- **CPU**: Intel(R) Core(TM) i9-14900HX
- **RAM**: 31.7 GB
- **屏幕分辨率**: 2560x1440
- **FFmpeg**: 4.2.1（预编译，avcodec-58 / avformat-58 / avutil-56）
- **SDL2**: 2.x（预编译）
- **构建**: MSVC 2017 x86 Release
- **RTSP Server**: 192.168.42.116:25544
- **音频设备**: 5 个 capture device 可用（Monster Mic 03 / Monster Mic 等）

## 2. 测试参数

| 参数 | 值 |
|------|-----|
| 采集分辨率 | 1920x1080 |
| 目标帧率 | 30fps |
| CRF | 23 |
| maxrate | 4Mbps |
| bufsize | 7M |
| preset | ultrafast |
| tune | zerolatency |
| max_b_frames | 0 |
| 音频 | AAC 48kHz 立体声 128kbps (SDL 默认设备) |
| 传输 | RTSP/TCP |
| RTSP URL | rtsp://192.168.42.116:25544/test |

## 3. 运行结果

### 3.1 概览

| 指标 | 值 | 来源 |
|------|-----|------|
| 总推流时长 | 600 秒 | CSV timestamp 差值 |
| 视频采集 fps（全时段平均） | 30.0 | CSV videoCaptured 增量计算 |
| 视频编码 fps（全时段平均） | 30.0 | CSV videoEncoded 增量计算 |
| 视频采集 fps（稳定段） | 30.0 | 跳过头尾 2 行后计算 |
| 视频编码 fps（稳定段） | 30.0 | 同上 |
| 丢帧数（累计） | 0 | CSV videoDropped 终值 |
| 音频采集字节（累计） | 232,369,920 | CSV audioBytes 终值 |
| AAC 编码帧数（累计） | 28,364 | CSV audioEncoded 终值 |
| RTSP 写包数（累计） | 46,514 | CSV packetsWritten 终值 |
| 写错误数（累计） | 0 | CSV writeErrors 终值 |
| 重连次数（累计） | 0 | CSV reconnects 终值 |

### 3.2 队列和耗时

| 指标 | 值 | 来源 |
|------|-----|------|
| rawQueue max | 1 | CSV 全时段 |
| encQueue max | 1 | CSV 全时段 |
| encodeMaxUs | 20,825 us (21ms) | CSV 全时段 max |
| muxWriteMaxUs | 34,117 us (34ms) | CSV 全时段 max |
| rawQueue 趋势 | 平稳 | 前后 25% 均为 1 |

### 3.3 异常统计

| 指标 | 值 | 来源 |
|------|-----|------|
| VBV underflow 次数 | 14 | 日志 grep |
| ERROR 行数 | 0 | 日志 grep |
| WARN 行数 | 0 | 日志 grep |
| write / send / network error | 0 | 日志 grep |
| 重连事件 | 0 | 日志 grep |

**VBV underflow 详情**:
- 首次: frame 3451，-152827 bits（约运行 115 秒时）
- 末次: frame 11491，-62616 bits（约运行 380 秒时）
- 全部 14 次均为 DEBUG 级别，未升级为 ERROR

### 3.4 码率

| 指标 | 值 |
|------|-----|
| bitrateKbps（CSV） | 0（已知问题：PusherStats::bitrateKbps 声明但从未更新） |

**估算码率**（从采集帧数和时长推算）：
- 视频: 10 分钟 18,000 帧，CRF 23；视频+音频 RTSP 写包 46,514 个
- 音频字节率: 232,369,920 / 600 ≈ 387 KB/s ≈ 3,099 kbps（原始 PCM）
- 实际输出码率需等 V2.1 实现统计后确认

### 3.5 关键事件时序

| 事件 | 时间戳 |
|------|--------|
| 程序启动 | 16:10:38.469 |
| SDL 音频设备枚举 | 16:10:38.573（5 个设备可用） |
| 首帧采集 | 约 16:10:38（startup 阶段） |
| 首个视频包编码完成 | 16:10:43.617（fps=28.2, 142 帧） |
| 首个 RTSP 写包成功 | 约 16:10:43（首个 mux 统计窗口） |
| 音频采集/编码开始 | 在首个 stats 窗口即已运行 |
| 首次 VBV underflow | 16:12:33.963（frame 3451） |
| 末次 VBV underflow | 16:17:02.011（frame 11491） |
| 停止 | 16:20:43（进程被外部终止） |

### 3.6 结束状态（最后一行数据）

| 指标 | 终值 |
|------|------|
| videoCaptured | 18,224 |
| videoEncoded | 18,208 |
| audioEncoded | 28,430 |
| packetsWritten | 46,585 |
| encodeMaxUs | 20,825 us |
| muxWriteMaxUs | 34,117 us |

## 4. 可接受项

1. **视频采集 fps 稳定 30.0**：全时段平均 30.0fps，无波动，无异常丢帧（videoDropped = 0）
2. **视频编码 fps 稳定 30.0**：与采集 fps 一致，编码耗时 21ms 远低于帧间隔 33ms
3. **音频采集和编码持续增长**：audioBytes 232MB，audioEncoded 28,364 帧，增势稳定无中断
4. **推流写包无错误**：writeErrors = 0，muxWriteMaxUs 34ms 可接受
5. **队列不持续增长**：rawQueue 恒定 1，encQueue 基本为 0（偶现 1）
6. **无崩溃、无重连、无 ERROR/WARN**：10 分钟运行完全稳定
7. **编码耗时可控**：encodeMaxUs 21ms，muxWriteMaxUs 34ms，不存在持续上升趋势

## 5. 不可接受项

1. **bitrateKbps 恒为 0（P0）**：PusherStats::bitrateKbps 已声明但从未在任何线程中更新，无法从 CSV 判断实际输出码率。需要在 V2.1 按统计窗口计算实际输出码率。
2. **VBV underflow 持续出现（P0）**：10 分钟内出现 14 次，从 frame 3451 到 11491 范围。当前 CRF 23 + maxrate 4M/bufsize 7M 组合未消除 underflow。需在 V2.3 处理编码器参数。
3. **无 A/V 同步基准（P0）**：AVSyncClock 文件不存在。视频 PTS 从 frameIndex 起始，音频 PTS 从 0 独立起始。无法观测 A/V 漂移。需在 V2.2 实现统一时钟。
4. **队列无最大值追踪**：CSV 中 rawQueue/encQueue 为采样瞬时值，非统计窗口内最大值。true max 可能在采样间隔之间发生但未被记录。
5. **音频 ring buffer 无统计**：CSV 无 audio overflow/underrun 字段，日志中音频编码线程序正常但无 ring buffer 水位信息。
6. **编码 maxUs 未区分窗口**：encodeMaxUs 和 muxWriteMaxUs 为全程累计 max，非窗口 max，无法观察耗时是否在恶化。

## 6. 结论

V1 主链路在 10 分钟测试中表现稳定：视频 30fps 无丢帧，音频持续采集编码，RTSP 写入零错误，无崩溃无重连。

V2 必须修复的 P0 问题：
1. **bitrateKbps 未实现** — V2.1 修复，将按统计窗口计算实际输出码率
2. **AVSyncClock 不存在** — V2.2 实现统一启动基准和 A/V offset 统计
3. **VBV underflow** — V2.3 优化编码参数组合消除或显著减少

建议按计划顺序推进：V2.1（可观测性补齐）→ V2.2（音视频同步）→ V2.3（低延时和码控）。
