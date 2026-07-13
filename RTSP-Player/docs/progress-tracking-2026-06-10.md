# RTSP-Player 进度跟进报告

**日期**: 2026-06-10  
**构建**: Release x86 (SDL2 v2)  
**测试环境**: 本地 RTSP 推流 → localhost  
**源流参数**: H.264 1280x720 29fps 1742kb/s + AAC 48000Hz stereo 171kb/s, 6'31" 循环

---

## 1. 测试概况

| 项目 | 数值 |
|------|------|
| 会话时长 | 111.7 秒 (21:44:38.440 → 21:46:30.113) |
| 解码总帧数 | 3,025 |
| 渲染总帧数 | ~2,820 |
| 主动丢帧 | 37+ (CSV 记录 32, 末尾 burst 未入 CSV) |
| 平均解码速率 | ~27.1 fps (源 29fps, 差距 1.9fps) |
| 首帧渲染延迟 | ~8.0s (open → 第一帧 Render #1) |
| PacketQueue 关键帧丢弃 | 47 次 (42ms 队列阈值处高频触发) |
| FFmpeg 解码错误 | 6 次 |
| 音频欠载峰值 | 15 次 / 5s 窗口 |
| 重连触发 | 0 次 (链路未中断) |

---

## 2. 启动时间线

```
21:44:38.440  Logger 启动
21:44:38.711  SDL 渲染器创建 (1280x720)
21:44:38.712  StreamLifecycleManager::open() 开始
21:44:38.712  首次 shutdownPipeline (清理旧状态)
21:44:40.206  avformat 连接完成 (~1.5s, 含 probesize/analyzeduration)
21:44:40.977  Video PacketQueue 初始化 (200ms, timeBase=11.11us)
21:44:40.978  Audio PacketQueue 初始化 (80ms, timeBase=20.83us)
21:44:41.539  4 线程启动 (Demux / VideoDecode / AudioWorker / SDL回调)
21:44:41.540  **PacketQueue 连丢 13 个关键帧** (启动突发, ~2ms 内)
21:44:46.620  首帧解码 (Decoded #0, pts=5.827s)
21:44:46.636  首帧渲染 (Render #1, pts=5.827s, latency=0)
```

**问题**:  
- `open()` 到首帧渲染 ~8.0s，距离"秒开"目标差距大。
- 启动时 demux 快速推送触发 13 帧连丢（解码线程尚未就绪，PacketQueue 无背压缓冲）。
- 音视频仅 "received=1" 就因关闭而停止，启动阶段的音频留存未充分验证。

---

## 3. 运行时性能分析

### 3.1 帧率追踪 (CSV 数据)

| 时间偏移 | 解码累计 | 渲染累计 | 丢帧累计 | 渲染间距avg | 渲染间距max |
|----------|----------|----------|----------|-------------|-------------|
| 5s | 144 | 144 | 0 | 34ms | 64ms |
| 10s | 289 | 289 | 0 | 34ms | 63ms |
| 15s | 434 | 430 | 4 | 35ms | 272ms |
| 20s | 578 | 572 | 6 | 35ms | 238ms |
| 25s | 724 | 718 | 6 | 34ms | 68ms |
| 30s | 868 | 796 | 22 | **63ms** | **1336ms** |
| 35s | 1014 | 942 | 22 | 34ms | 69ms |
| 40s | 1159 | 1087 | 22 | 34ms | 77ms |
| 45s | 1304 | 1231 | 22 | 34ms | 68ms |
| 50s | 1594 | 1454 | 30 | **52ms** | **2393ms** |
| 55s | 1739 | 1599 | 30 | 34ms | 56ms |
| 60s | 1884 | 1744 | 30 | 34ms | 66ms |
| 65s | 2029 | 1889 | 30 | 34ms | 69ms |
| 70s | 2174 | 2034 | 30 | 34ms | 66ms |
| 75s | 2319 | 2177 | 32 | 34ms | 147ms |
| 80s | 2463 | 2321 | 32 | 34ms | 60ms |
| 85s | 2609 | 2467 | 32 | 34ms | 71ms |
| 90s | 2754 | 2612 | 32 | 34ms | 62ms |
| 95s | 2899 | 2757 | 32 | 34ms | 61ms |

**模式**: 正常期渲染间距 ~34ms (稳定 29fps)；每~30s 出现一次渲染间距尖峰 (272ms → 1336ms → 2393ms → 147ms)，伴随 8 帧 burst 丢帧。

### 3.2 延迟尖峰事件（日志精确）

| 时间 | PTS | 延迟 | 丢帧 burst | 说明 |
|------|-----|------|-----------|------|
| 21:44:57.348 | 16.3s | 220ms | 2 | 第一次小尖峰 |
| 21:44:58.515 | 17.5s | 213ms | 2 | 紧接第二次 |
| 21:45:04.149 | 23.2s | 196ms | 2 | |
| **21:45:13.304** | **31.2s** | **1,282ms** | **8** | 🔴 严重卡顿 #1 |
| **21:45:15.298** | **33.6s** | **929ms** | **8** | 🔴 严重卡顿 #2 |
| **21:45:37.129** | **54.0s** | **2,343ms** | **8** | 🔴 严重卡顿 #3 |
| 21:46:05.814 | 84.9s | 108ms | 2 | |
| 21:46:28.627 | 107.7s | 138ms | 2 | |
| 21:46:30.313 | 109.3s | 197ms | 1 | 关闭前最后一帧 |

**结论**: 3 次严重卡顿（延迟 >900ms）间隔 ~21s 和 ~20s，似有周期性规律。每次恢复后约 2 秒正常播放，然后再次卡顿。

---

## 4. 瓶颈根因分析

### 4.1 周期性渲染管线卡顿 (🔴 严重)

**现象**: 每 20-30 秒出现一次 1-2.3s 渲染延迟尖峰，触发 8 帧 burst 丢帧。

**可能根因**:
1. **解码线程阻塞**: VideoDecodeThread 在 `avcodec_receive_frame` / `avcodec_send_packet` 中发生长时间等待（IDR 帧等待、参考帧重建）。
2. **PacketQueue 背压失效或统计不足**: 复核当前 Release 源码后，视频 `PacketQueue` 已是 1000ms，并非初稿中写的 200ms。仍然发生关键帧丢弃，说明问题不是单纯容量不足，而是生产/消费失衡、突发读取或队列统计口径不足。
3. **RenderScheduler 已移除**: v2 架构中主循环 1ms tick 直接检查帧可用性（无 CV 等待），但 `videoRefresh()` 中的 A/V sync 等待逻辑使用 `delay + drift` 计算，delay 上限 500ms —— 期间解出的帧会堆积在 VideoFrameQueue (8 slot) 中，一旦超出 slot 数就丢失。
4. **VideoFrameQueue 容量不是唯一根因**: 复核当前 Release 源码后，`VideoFrameQueue` 已是 8-slot，并非初稿中写的 4-slot。8-slot 仍无法吸收 >900ms 尖峰，因此应先记录 `writeFrame()` 失败、队列深度和同步等待，再决定是否继续扩容。

**推测链**:
```
解码线程被阻塞 (IDR 等待/重建)
  → 帧产出暂停 ~1-2s
  → render 无新帧可显 → video clock 停止推进
  → demux 继续推送 → PacketQueue 满 → 丢关键帧
  → 阻塞解除 → 解码产出 burst → VideoFrameQueue 溢出或同步追赶 → 丢帧 burst
  → render 追赶 → 渲染间距尖峰
```

### 4.2 PacketQueue 拥塞恶化 (🔴 严重)

**现象**: 60 秒后 PacketQueue 关键帧丢弃频率飙升至 1-5 次/秒，持续到程序关闭。

**数据**:
| 时间段 | 关键帧丢弃次数 | 频率 |
|--------|---------------|------|
| 0-60s | 15 | ~0.25/s |
| 60-112s | 32 | ~0.62/s |

**根因**: 解码线程消费速率持续低于 demux 生产速率（源 29fps → 实际解码 ~27fps）。累积时延导致 PacketQueue 持续满。每次关键帧丢弃后需等下一个 IDR 才能恢复解码，进一步恶化。

### 4.3 音频链路欠载 (🟡 中等)

**现象**:
- 60s 后最多 15 次音频欠载 / 5s 窗口
- 关闭时 `Audio: queue empty 30s, flushing`
- `Audio worker stopped (received=1)` 实际来自布尔值 `hasAudio`，只能说明曾经产出过音频，不能证明只收到 1 个音频包

**根因**: 音频链路仍然未充分验证，但 `received=1` 不是有效计数器。真正需要追的是 SDL 回调读空、ring buffer 填充深度和音频包/帧/写入计数。可能是：
1. AudioWorker 缺少真实 `packetsReceived / framesDecoded / bytesWritten` 计数，导致日志不可判读
2. AudioRingBuffer 在 60s 后被 SDL 回调持续读空，触发 `audioUnderrun`
3. 音频欠载可能是 demux/decode 周期性 stall 的连带症状，而非独立音频 bug

**注意**: CSV 显示 60s 后有非零 audioUnderrun 计数（15, 1, 8, 6, 10, 6, 1）。关闭时 "queue empty 30s" 表明关闭前音频队列干涸，但仍需新增真实计数器后才能判断是音频生产不足还是全链路 stall。

### 4.4 FFmpeg 解码错误 (🟡 中等)

```
error while decoding MB 57 15, bytestream -12     (启动时)
error while decoding MB 15 15, bytestream -20     (运行中)
left block unavailable for requested intra4x4     (54s)
left block unavailable for requested intra mode   (84s)
co located POCs unavailable                       (启动时×2)
mmco: unref short failure                         (启动时)
```

**性质**: H.264 宏块解码错误和参考帧管理问题。可能是源流 `demo.flv` 文件编码质量问题（非推流/拉流链路问题）。对播放影响较小（FFmpeg 内部容错）。

### 4.5 启动背压缺失 (🟡 中等)

**现象**: 启动时 demux 13 帧连丢（21:44:41.540 同一毫秒内）。原因是 `avformat_open_input` 返回后 demux 立即开始快速读取缓冲数据，而 VideoDecodeThread 尚未准备好消费（解码器刚 open，需等第一个 IDR）。

**建议**: 在 `startThreads()` 后添加同步点（如 `condition_variable`），确保解码线程初始化完成后再启动 demux 线程。

---

## 5. Phase 完成度评估

| Phase | 目标 | 完成度 | 判定 |
|-------|------|--------|------|
| 1 | Video 秒开链路 | 80% | ⚠️ 启动同步完成，视频启动丢帧 13→0，但首帧仍约 8.8s。SDL 窗口渲染正常。 |
| 2 | Frame Drop + A/V Sync | 60% | ⚠️ 丢帧机制工作（50ms 阈值 + 2帧 burst 恢复），但延迟累积导致大量关键帧丢弃和 burst 丢帧。A/V sync 基础计算正确。 |
| 3 | Reconnect + Serial | 未测试 | ❌ 本次运行无断线事件。Serial 机制未在真实重连场景验证。 |
| 4 | Audio 链路 | 45% | ⚠️ 部分完成，观测字段仍有可信度缺陷（数据竞争/统计口径问题）。60s 后持续欠载。 |
| 5 | A/V Sync 精细调优 | 0% | ❌ 未开始。长期播放偏差 >30ms（目标），延迟尖峰频繁。 |
| 6 | 生产化打磨 | 20% | ❌ 仅 2 分钟运行（目标 30 分钟）。CSV 统计框架可用但数据不足。无 resize/fullscreen 测试，无内存泄漏检查。 |

---

## 6. 复核结论与下一步路线图（修订版）

### 6.1 复核修正

1. **队列容量判断需修正**: 当前 Release 源码中视频 `PacketQueue` 已初始化为 1000ms，初稿中的 "200ms → 400-600ms" 建议已过时。继续盲目扩容可能只会增加延迟，不能解释为什么 1000ms 下仍有关键帧丢弃。
2. **FrameQueue slot 判断需修正**: 当前 `VideoFrameQueue::kSlotCount` 已是 8，初稿中的 "4-slot 太浅" 已过时。8-slot 仍有 burst 丢帧，说明应先定位 decode stall、sync wait 和 `writeFrame()` 失败，再决定是否扩容。
3. **音频 `received=1` 判断需修正**: `AudioWorker` 退出日志中的 `received=1` 是布尔 `hasAudio`，不是音频包计数。音频问题的可靠证据是 60s 后的 `audioUnderrun` 峰值，以及关闭前 "queue empty 30s"。
4. **CSV 队列深度证据不足**: Release CSV 中 `vQueuePeak/aQueuePeak` 基本为 0，无法证明队列真实高水位。下一轮必须补队列深度和 ring buffer 填充度埋点。

### 6.2 更新后的优先级

#### P0 — 先让数据可信

1. **补观测埋点**: 在 `VideoDecodeThread` 记录 `avcodec_send_packet` / `avcodec_receive_frame` 单次耗时、连续耗时尖峰和错误码；在 `VideoFrameQueue::writeFrame()` 记录写失败次数和当前 slot 占用；在 `PacketQueue` 记录 push/pop 后 duration/size 高水位。
2. **修正音频计数器**: 将 `AudioWorker` 的 `received` 布尔日志改成真实 `packetsReceived / framesDecoded / bytesWritten`，并记录 `AudioRingBuffer` 当前填充、读空次数和写阻塞次数。
3. **标记 sync 决策原因**: 在 `videoRefresh()` 对 `delay/drift/currentPts/latency/drop reason` 做低频日志，尤其记录 latency >50ms 和 burst drop 发生时的上下文。

#### P1 — 修启动链路

4. **线程启动同步**: `startThreads()` 中不要让 demux 先无约束读取。先启动 video decode / audio worker，等待它们进入 ready 状态后再放开 demux；或给 demux 增加 started barrier。
5. **启动阶段验收**: 以同一条 localhost RTSP 流重跑 Release，目标是 `open()` 到首帧 <1s，启动阶段 keyframe drop 为 0，首个 CSV 采样中 decoded/rendered 接近源帧率。

#### P1.5 — 数据可信度修复

6. **修复统计口径与数据竞争**: `PacketQueue` 增加队列名日志和 `drainPeak()`；`AudioWorker` 周期性写入真实 `packetsReceived/framesDecoded/bytesWritten` 计数器；`AudioRingBuffer` 指标改成线程安全快照（`atomic` 或锁内 drain）。
7. **标记队列来源**: `PacketQueue` 日志增加 `name/streamType` 字段，消除"音频队列丢弃"无法确认的歧义。

#### P2 — 定位周期性卡顿

8. **区分 decode-bound 与 sync-bound**: 若 stall 窗口内 `receive_frame` 耗时突增，优先查源流 GOP/IDR、解码器线程和丢参考帧；若 decode 正常但 render 等待/丢帧，优先修 A/V sync 和 burst recovery 策略。
9. **谨慎调阈值**: 只有在确认不是 decode 阻塞后，再调整 50ms drop 阈值、burst recovery 和 `delay` 上限。当前不建议先扩队列或先放宽阈值。

#### P3 — 音频链路验证

10. **确认音频欠载性质**: 对比 video stall 时间窗与 `audioUnderrun` 时间窗，判断音频是被同一 stall 拖垮，还是 AudioWorker / RingBuffer 独立生产不足。
11. **音频验收**: 目标是在本地 RTSP 场景下 10 分钟运行 `audioUnderrun` 接近 0，或能明确指出欠载来自上游 stall。

#### P4 — Release 稳定性矩阵

12. **30 分钟连续播放**: 观察长期漂移、内存增长、keyframe drop 是否失控、paint interval 是否持续稳定。
13. **Reconnect 验证**: 手动断流/恢复，验证 `Recovering → Reconnecting → Playing`、serial 丢弃旧帧、音视频队列清理。
14. **窗口与关闭路径**: 验证 resize/fullscreen、ESC/q 退出、关闭时线程 join 与日志关闭顺序。

### 6.3 建议执行顺序

```
修复统计口径/数据竞争 → 标记队列来源 → 再做音频队列参数实验 → 30 分钟复测
```

**下一步建议**: 先做 P1.5 数据可信度修复（修复统计口径/数据竞争 → 标记队列来源），再做音频队列参数实验（80ms→200ms，30 分钟复测）。不要先继续调大队列或放宽丢帧阈值，因为当前 Release 已经是视频队列 1000ms、FrameQueue 8-slot，盲调参数无法解释现有尖峰。

---

## 7. 日志文件索引

- 完整日志: `bin/MSVC2017_x86_Release/rtsp_player.log` (277 行)
- 统计 CSV: `bin/MSVC2017_x86_Release/stats.csv` (22 行, 5s 采样间隔)
- Debug 日志 (旧): `bin/MSVC2017_x86_Debug/rtsp_player.log` (936KB) — 含更早的运行数据

---

*本文档基于 2026-06-10 Release x86 构建的单次 112 秒测试运行。后续运行数据应追加到此文档或扩展为新节。*
