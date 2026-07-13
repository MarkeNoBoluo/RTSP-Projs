# P0+P1 埋点验证报告

**日期**: 2026-06-11  
**构建**: Release x86 (SDL2)  
**测试流**: `rtsp://127.0.0.1:25544/2026_06_10` (H.264 1280x720 29fps + AAC 48000Hz stereo)  
**测试依据**: `docs/progress-tracking-2026-06-10.md` 第 6.2 节 P0+P1 优先级

---

## 1. 测试概况

| 项目 | 数值 |
|------|------|
| 运行时长 | ~43 分钟 (2586s) |
| 视频解码总帧数 | 74,998 |
| 视频渲染总帧数 | 74,675 |
| 视频丢帧 | 213 (0.28%) |
| 音频收包 | 119,923 包 → 119,923 帧 → 491,204,608 字节 PCM |
| PacketQueue 关键帧丢弃 | 1,570 次 (全部在音频队列) |
| FFmpeg 解码错误 | 137 次 |
| 慢解码事件 (send>50ms) | 0 次 |
| 重连 | 0 次 |

---

## 2. 启动时间线

```
23:26:42.949   Logger 启动
23:26:43.255   open() 开始
23:26:43.257   shutdownPipeline (清理旧状态)
23:26:46.265   音视频流信息获取完成
23:26:46.268   解码器打开
23:26:46.745   SDL 音频设备打开
23:26:46.745   视频解码线程就绪 (0ms)
23:26:46.751   音频 worker 就绪 (5ms)         ← P1-1 同步点
23:26:46.751   Demux 线程启动                  ← 在解码线程就绪后
23:26:46.847   视频解码首次 pop timeout (队列尚未有数据)
23:26:46.905   音频首帧解码 + 写入 RingBuffer
23:26:46.905-906 音频 PacketQueue 连丢 4 个关键帧 (queue=42ms)
23:26:52.059   首帧解码 (pts=5.827s)
23:26:52.073   首帧渲染 (pts=5.827s, latency=0us)
```

**结论**: `open()` → 首帧渲染 ~8.8s，瓶颈在 `avformat_open_input` (约 3.0s) + 首帧 PTS 偏移 (源流从 5.8s 开始)。

---

## 3. P0 埋点验证

### 3.1 P0-1: VideoDecodeThread 解码耗时与错误计数 ✅

| 指标 | 结果 |
|------|------|
| `send_packet` 单次峰值 (退出日志) | 1,099 us |
| `receive_frame` 单次峰值 (退出日志) | ~0 us (极快) |
| `send_packet` 错误 | 0 |
| `receive_frame` 错误 | 0 |
| 慢 send (>50ms) | 0 次 |
| 慢 receive (>50ms) | 0 次 |
| CSV 每间隔 sendMax | 517~9,710 us (因 CSV 写入竞态，退出时清零) |

**评估**: 解码管线无阻塞，`send_packet`/`receive_frame` 耗时均为微秒级。CSV 中 `decodeReceiveUsMax` 偶尔出现 0 是由于 `writeCsvRow()` 在解码线程退出前重置了该字段——属于 design issue 而非功能缺失。

### 3.2 P0-2: VideoFrameQueue 写失败计数 ✅

| 指标 | 结果 |
|------|------|
| 运行时写失败累计 | ~10 次 |
| 退出 flush 阶段写失败 | 14 次 (解码器 flush 输出 → queue 已停止消费) |
| CSV `fqWriteFail` | 行 174: 3 次, 行 220: 1 次, 行 445: 5 次 |

**评估**: 运行时写失败极少（对应 burst 丢帧场景），正常。

### 3.3 P0-3: PacketQueue 高水位 ⚠️

| 指标 | CSV 表现 | 问题 |
|------|----------|------|
| `vQueuePeakMs` | **恒为 275** (全部 519 行) | `m_peakDurationMs` 是终生高水位，未被每间隔重置 |
| `vQueuePeakPkts` | **恒为 8** (全部 519 行) | 同上 |
| `aQueuePeakMs` | 大部分为 0，偶见 33 | 音频队列较小(80ms)，峰值采样概率低 |

**根因**: `PacketQueue::m_peakDurationMs` 和 `m_peakSize` 只在 `push()` 中递增，从不重置。`videoRefresh()` 每 30 帧调用 `peakDurationMs()` 读取同一终生峰值 → CSV 每行显示相同值。

**结论**: P0-3 的峰值追踪逻辑正确（确实记录了高水位 275ms），但 **CSV 视角不可用**，因为无法区分各 5s 间隔的峰值变化。需要增加 `drainPeak()` 或在采样后重置。

### 3.4 P0-4: AudioWorker 真实计数器 ⚠️

| 字段 | CSV 值 | 退出日志值 | 问题 |
|------|--------|-----------|------|
| `aPktRecv` | 全部为 0 | 119,923 | 值仅在 `AudioWorker::run()` 退出时写入 `m_stats` |
| `aFrmDec` | 全部为 0 | 119,923 | 同上 |
| `aBytesWrit` | 全部为 0 | 491,204,608 | 同上 |

**根因**: `AudioWorker::run()` 用局部变量 `packetsReceived`/`framesDecoded`/`bytesWritten` 计数，仅在退出时间步写入 `PlayerStats`。CSV 每 5s 读取这些 `atomic` 字段时始终读到 0。

**结论**: 退出日志数据正确可用，但 **CSV 运行时追踪完全缺失**。需要改为周期性写入（如每 100 帧）。

### 3.5 P0-5: AudioRingBuffer 指标 ✅ (部分)

| 指标 | 行为 |
|------|------|
| `aRingFill` | 0~65,536 字节波动，正常 |
| `aRingEmpty` | 累积到 1,611 次 (SDL 回调读空总次数) |
| `aRingBlocked` | **始终为 0** — AudioWorker 从未因 RingBuffer 满而阻塞 |

**评估**: 指标可用。`aRingBlocked=0` 结合 `aRingEmpty=1611` 说明音频管线是 **生产不足**（上游 PacketQueue 丢包 → RingBuffer 干涸），而非消费阻塞。

### 3.6 P0-6: videoRefresh() Sync 决策日志 ✅

丢帧日志现在包含完整上下文：

```
Drop frame pts=86.965s latency=658529us drift=625179us burst=1 (delay=500.0ms)
Drop frame pts=120.483s latency=412972us drift=580256us burst=1 (delay=500.0ms)
```

**关键发现**: `delay` 频繁被钳制在 500ms 上限。drift 在 570~630ms 之间，说明音频时钟领先视频约 600ms → 导致大量 `latency > 50ms` 触发丢帧。延迟尖峰期间 burst 可达 29 帧。

---

## 4. P1 启动同步验证

### 4.1 P1-1: 线程启动同步 ✅

| 对比项 | 旧版 (6/10 报告) | 新版 (P1 修复后) |
|--------|-----------------|-----------------|
| 启动顺序 | Demux → VideoDecode → AudioWorker | VideoDecode → AudioWorker → **(等就绪)** → Demux |
| 视频解码线程就绪 | N/A (无同步) | 0ms |
| 音频 Worker 就绪 | N/A (无同步) | 5ms |
| 启动阶段视频队列关键帧丢弃 | **13 帧** (2ms 内) | **0 帧** ✅ |
| 启动阶段音频队列关键帧丢弃 | N/A | 4 帧 |

**评估**: P1-1 消除了视频队列的启动突发丢帧（13→0）。音频队列 4 次丢弃是队列容量问题（80ms 太小），非同步问题。

---

## 5. 运行时瓶颈分析（依据新埋点数据）

### 5.1 核心瓶颈：音频 PacketQueue 容量不足

- 音频 PacketQueue 容量: **80ms**
- 每个 AAC 帧时长: **21.3ms** (1024 samples / 48000 Hz)
- 80ms 队列仅能容纳 **3~4 个音频包**
- 1,570 次关键帧丢弃全部发生在音频队列（日志均显示 `queue=42ms`）
- 丢弃后队列剩余 42ms（= 2 个包），说明每次丢弃从 63ms 减至 42ms

```
Demux 生产速率 > AudioWorker 消费速率
  → 音频 PacketQueue(80ms) 满
  → 丢关键帧（所有音频包都有 KEY flag）→ AAC 解码需要 IDR 恢复
  → AudioWorker 解码停滞
  → RingBuffer 读空 (aRingEmpty: 1611 次)
  → SDL 回调拿到静音 → 音频欠载 → drift 漂移 >600ms
  → videoRefresh 检测到 latency 尖峰 → burst 丢帧 (29 帧)
```

### 5.2 视频管线状态：健康

| 指标 | 值 | 评估 |
|------|-----|------|
| 视频 PacketQueue 峰值 | 275ms / 1000ms (27.5%) | 余量充足 |
| 慢解码 (send/receive >50ms) | **0 次** | 无解码阻塞 |
| 解码错误 | 0 (非 FFmpeg 内部错误) | 容错良好 |
| FrameQueue 写失败 | 10 次 / 75,000 帧 | 极低 |

### 5.3 周期性卡顿模式

与 6/10 报告对比：

| 指标 | 6/10 报告 | 本次 |
|------|----------|------|
| 关键帧丢弃 | 47 次 | 1,570 次 |
| 严重卡顿 (>900ms) | 3 次 / 112s | 减少（本次多数为 100-600ms 级别） |
| 音频欠载 | 15 次/5s 峰值 | 最大 29 次/5s |

丢弃数大幅上升的原因是 **运行时长从 2 分钟延长到 43 分钟**——音频队列持续满溢导致累积丢帧。单位时间丢帧率基本一致。

---

## 6. 发现的数据可信度缺陷 (待修)

| # | 严重度 | 问题 | 影响 |
|---|--------|------|------|
| 1 | 🔴 | `vQueuePeakMs`/`vQueuePeakPkts` 终生不重置 → CSV 每行相同 | 丢失各 5s 间隔的队列峰值变化 |
| 2 | 🔴 | `aPktRecv`/`aFrmDec`/`aBytesWrit` 仅在退出时更新 → CSV 全 0 | 运行时音频计数器完全不可用 |
| 3 | 🟡 | `decodeReceiveUsMax` 可能被 `writeCsvRow()` 提前清零 | 偶尔丢失解码峰值 |
| 4 | 🟡 | `aRingFill`/`aRingEmpty`/`aRingBlocked` 通过 `videoRefresh()` 每 30 帧采样，粒度粗 | 尚可接受 |

---

## 7. 建议下一步优先级

### 立即修复 (下一轮前)

1. **扩大音频 PacketQueue → 200-500ms**，消除 80ms 瓶颈导致的 1570 次关键帧丢弃
2. **修复 bug #1**: PacketQueue 增加 `drainPeak()` 方法，采样后重置峰值
3. **修复 bug #2**: AudioWorker 每 100 帧写入一次 `PlayerStats` 计数器

### 中优先级 (P2)

4. **音频 drift 问题**: 当前 drift 稳定在 570~630ms，表现为 videoRefresh 中 delay 始终被 500ms 上限钳制。需排查是 AudioWorker 解码落后还是 SDL callback 读取落后。
5. **A/V sync 修复**: burst 丢帧后应重置 drift 或强制重锚，防止 29 帧连续丢弃。
6. **long-run 验收**: 30 分钟 + reconnect 测试，观察内存增长和 CSV `aRingEmpty` 失控。

---

*本文档基于 2026-06-11 Release x86 构建的单次 43 分钟运行。*
