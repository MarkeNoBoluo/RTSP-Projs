# RTSP-Pusher 优化方案

## 1. 结论

当前版本主链路已经能长时间运行，但不建议直接作为 PCS-Service 的正式推流服务模块集成。建议先补齐可观测性，再优化码控，最后验证服务化生命周期。

本方案基于以下测试产物：

- `bin/MSVC2017_x86_Release/log_062001.log`
- `bin/MSVC2017_x86_Release/csv_062001.csv`

## 2. 本次测试摘要

| 指标 | 结果 |
|------|------|
| 测试时长 | 约 57 分 05 秒 |
| 视频采集平均 FPS | 29.986 |
| 视频编码平均 FPS | 29.986 |
| videoDropped | 0 |
| writeErrors | 0 |
| reconnects | 0 |
| rawQueue 最大值 | 1 |
| encQueue 最大值 | CSV 18，日志峰值 26 |
| mux 队列日志峰值 | 23 |
| encodeMaxUs | 24.477 ms |
| muxWriteMaxUs | 320.042 ms |
| VBV underflow | 3519 次 |
| bitrateKbps | 全程 0 |

可接受项：

- 视频采集和编码接近 30fps。
- 没有视频丢帧。
- RTSP 写包没有错误。
- 没有重连。
- 日志中没有 `[WARN]` 和 `[ERROR]` 级别事件。
- rawQueue 没有积压。

不可接受项：

- `bitrateKbps` 仍然恒为 0，无法判断真实输出码率。
- `VBV underflow` 贯穿全程，码控不稳定。
- `muxWriteMaxUs` 出现 320ms 尖峰。
- `encodeMaxUs` 和 `muxWriteMaxUs` 是全程累计最大值，不能反映窗口趋势。
- 当前日志没有 `Thread stopped`、`RTSP trailer written`、`Logger closed`，本次测试不能证明优雅退出路径可靠。
- 缺少 A/V offset 和 PTS/DTS 异常统计。

## 3. 优化原则

1. 先补统计，再调参数。
2. 先用 CSV 和日志证明问题，再修改业务逻辑。
3. 每个阶段都要有可复现测试结果。
4. PCS-Service 集成前必须验证启动、停止、重启、断流恢复和资源释放。
5. 不为消除少量重复提前抽象复杂框架。

## 4. 阶段一：可观测性补齐

目标：让 CSV 和日志能直接判断推流质量。

### 4.1 必改字段

| 字段 | 处理方式 |
|------|----------|
| `bitrateKbps` | 在 mux 写包成功后累计 packet size，按 5 秒窗口计算输出码率 |
| `videoPacketCount` | mux 层统计视频包数量 |
| `audioPacketCount` | mux 层统计音频包数量 |
| `windowEncodeMaxUs` | 统计最近窗口内视频编码最大耗时 |
| `windowMuxWriteMaxUs` | 统计最近窗口内 mux 写包最大耗时 |
| `rawQueueMax` | 统计最近窗口内 rawQueue 峰值 |
| `encQueueMax` | 统计最近窗口内 encQueue 峰值 |
| `vbvUnderflowCount` | 从 FFmpeg log callback 中识别 `VBV underflow` 并计数 |
| `audioRingBytes` | 记录音频 ring buffer 当前水位 |
| `audioOverflowCount` | 统计音频 ring buffer 写入溢出 |
| `audioUnderrunCount` | 统计音频 ring buffer 读取不足 |

### 4.2 日志要求

日志中需要明确输出统计窗口，例如：

```text
statsWindowSec=5
```

周期性日志建议包含：

```text
bitrateKbps=...
videoPackets=...
audioPackets=...
windowEncodeMaxUs=...
windowMuxWriteMaxUs=...
rawQueueMax=...
encQueueMax=...
vbvUnderflowCount=...
```

### 4.3 验收标准

- `bitrateKbps` 不再恒为 0。
- CSV 每 5 秒能看出码率、队列、耗时趋势。
- 10 分钟测试后能直接判断是否存在队列积压、码率异常、写包尖峰。
- 日志和 CSV 字段含义同步更新到设计文档。

## 5. 阶段二：码控优化

目标：显著降低或消除 `VBV underflow`。

当前配置：

```text
1920x1080@30
bitrate=4000000
maxrate=4000000
bufsize=7000000
crf=23
preset=ultrafast
tune=zerolatency
gop=30
```

风险判断：

- 继续使用 `CRF + maxrate` 不适合作为服务默认配置。
- 服务模块更需要稳定码率和可预测延迟。
- 应优先验证 ABR/CBR 思路。

### 5.1 参数测试矩阵

| 方案 | bitrate | maxrate | bufsize | CRF | 目的 |
|------|---------|---------|---------|-----|------|
| A | 4M | 4M | 8M | 不设 | 稳定 ABR 基线 |
| B | 6M | 6M | 12M | 不设 | 判断 1080p 桌面内容是否带宽不足 |
| C | 8M | 8M | 16M | 不设 | 高码率对照组 |

### 5.2 x264 参数建议

固定以下参数：

```text
preset=veryfast 或 superfast
tune=zerolatency
keyint=30
min-keyint=30
scenecut=0
bframes=0
force-cfr=1
```

暂不默认启用 `nal-hrd`，需要结合 RTSP server 和播放端行为单独验证。

### 5.3 验收标准

- 10 分钟测试中 `VBV underflow` 显著下降，目标为 0。
- `bitrateKbps` 接近目标码率，允许合理波动。
- `rawQueue` 和 `encQueue` 不持续增长。
- `muxWriteMaxUs` 没有持续恶化。
- 播放端延迟不随时间明显累积。

## 6. 阶段三：A/V 同步观测

目标：不要只靠主观看音画是否同步。

### 6.1 新增字段

| 字段 | 含义 |
|------|------|
| `videoPtsMs` | 最近视频包 PTS 换算时间 |
| `audioPtsMs` | 最近音频包 PTS 换算时间 |
| `avOffsetMs` | `audioPtsMs - videoPtsMs` |
| `ptsErrorCount` | PTS/DTS 非单调或异常次数 |

### 6.2 实现要点

- pipeline 启动时建立统一 `startUs`。
- 视频帧保留 `captureTimeUs`。
- 音频采样块记录对应采集时间。
- reconnect 后明确重置首包 PTS、serial 和同步基准。

### 6.3 验收标准

- 10 分钟无 non-monotonic PTS/DTS。
- `avOffsetMs` 不持续单向扩大。
- 播放端主观检查无明显音画不同步。

## 7. 阶段四：服务化生命周期验证

目标：集成 PCS-Service 前验证启动、停止、重启不会留下线程和资源。

### 7.1 必测场景

| 场景 | 标准 |
|------|------|
| 正常启动 | 能进入 Streaming |
| 正常停止 | 日志出现 `Thread stopped`、`RTSP trailer written`、`Logger closed` |
| 连续启动停止 10 次 | 无崩溃、无线程卡死 |
| RTSP server 中断 | 能触发错误和恢复流程 |
| reconnect 后继续推流 | 播放端画面和声音恢复 |
| reconnect 后队列 | rawQueue / encQueue 被清理或不污染新流 |
| PCS-Service 停止服务 | 推流模块完整释放资源 |

### 7.2 验收标准

- stop/join 顺序明确。
- 退出时不会卡在 mux、encode、capture 或 audio 线程。
- reconnect 后 serial 递增，旧包被丢弃。
- 连续 30 分钟运行无队列持续增长、无未解释错误。

## 8. PCS-Service 集成建议

不要让 PCS-Service 直接依赖当前 exe 的交互模式。建议抽成服务模块接口：

```cpp
bool start(const PusherConfig& config);
void stop();
PusherState state() const;
PusherStatsSnapshot stats() const;
void requestReconnect();
```

PCS-Service 负责管理：

- RTSP URL。
- 音频设备选择。
- 日志路径。
- CSV 路径。
- 推流配置。
- 服务健康检查。
- 自动重启策略。

RTSP-Pusher 模块负责：

- 采集。
- 编码。
- mux 写入。
- 内部队列和线程生命周期。
- 可观测统计。
- 错误上报。

## 9. 推荐执行顺序

1. 修复 `bitrateKbps`、窗口统计、VBV 计数。
2. 跑一次 10 分钟基线，确认 CSV 可信。
3. 执行 A/B/C 三组码控测试。
4. 选择一组默认参数，再跑 30 分钟。
5. 增加 A/V offset 和 PTS/DTS 异常统计。
6. 做 stop/restart/reconnect 生命周期测试。
7. 接入 PCS-Service，先作为实验模块运行。
8. 完成 30 分钟和 60 分钟真实部署拓扑测试后，再作为正式服务模块启用。

## 10. 阶段交付物

| 阶段 | 交付物 |
|------|--------|
| 可观测性补齐 | 更新后的 CSV 字段、日志字段、字段说明文档 |
| 码控优化 | A/B/C 测试报告、推荐默认编码参数 |
| A/V 同步观测 | `avOffsetMs` 统计、PTS/DTS 异常统计 |
| 服务化生命周期 | start/stop/restart/reconnect 测试报告 |
| PCS-Service 集成 | 服务模块接口、集成说明、真实部署测试报告 |

