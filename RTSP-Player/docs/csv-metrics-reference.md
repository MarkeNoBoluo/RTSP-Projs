# CSV Metrics Reference

> RTSP-Player 每 5 秒输出的 CSV 统计字段说明文档，基于源码 `src/PlayerStats.cpp`、`src/PlayerStats.h` 及相关写入点分析生成。

## 1. CSV 基本信息

| 项目 | 说明 |
|------|------|
| 采样间隔 | 5 秒（`EVENT_STATS` SDL 定时器，在 `main.cpp` 中注册） |
| 输出路径 | 默认 `rtsp_player_stats.csv`，通过 `--csv <path>` 自定义 |
| 禁用方式 | `--no-csv` |
| 编码 | UTF-8（无 BOM），逗号分隔，CRLF 行尾 |
| 写入模式 | 追加（`std::ios::app`），每次启动新 session ID |

### 1.1 数据性质分类

CSV 中每个字段的数值含义分为三类：

- **累积值**：自进程启动以来的累计数值（`load()` 读取，不重置）
- **增量**：自上次 CSV 行以来的峰值或计数（`exchange(0)` 原子读取并清零）
- **快照**：写入 CSV 瞬间的瞬时值

### 1.2 前两列

| 列名 | 数据性质 | 说明 |
|------|----------|------|
| `session` | 标识 | 进程启动时间戳，格式 `YYYYMMDD_HHMMSS`（`localtime`） |
| `elapsed_s` | 快照 | `time(nullptr)` 返回的 Unix 时间戳。**命名有误导性**：该字段是挂钟时间而非启动后的经过秒数 |

---

## 2. 字段分类与逐一说明

### 2.1 帧计数器

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `vFrmDec` | 视频帧解码数 | 帧 | 累积 | `framesDecoded` |
| `vFrmRend` | 视频帧渲染数 | 帧 | 累积 | `framesRendered` |
| `vFrmDrop` | 视频帧丢弃数 | 帧 | 累积 | `framesDropped` |
| `vDropRate_pct` | 视频丢帧率 | % | 计算值 | `(vFrmDrop × 100) / vFrmDec` |

**正常范围**：
- `vFrmDrop`：稳定网络下约等于 0；每 5 秒有 1-5 帧丢弃属于可接受范围
- `vDropRate_pct`：< 1% 为正常；1%-5% 需关注；> 5% 表示严重问题

**异常含义**：
- 高丢帧率说明解码/渲染跟不上流的生产速率，或 A/V 同步追赶中大量丢弃
- 丢帧来源有三类：① `VideoFrameQueue` 满时覆盖写入；② `catchUpDrops`（A/V 同步追赶）；③ 单帧延迟丢帧（`DROP_THRESHOLD_US` 超时）

**数据来源**：
- `framesDecoded`：`VideoDecodeThread::run()` 中每次 `avcodec_receive_frame` 成功后递增
- `framesRendered`：`RTSPlayer::videoRefresh()` 渲染路径中递增
- `framesDropped`：三个写入点——`VideoFrameQueue::writeFrame()` 覆盖旧帧时、`RTSPlayer::videoRefresh()` 追赶丢帧时、`RTSPlayer::videoRefresh()` setpts-zero 模式下

---

### 2.2 音频计数器

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `aPktIn` | 音频包接收数 | 个 | 累积 | `audioPacketsReceived` |
| `aFrmDec` | 音频帧解码数 | 帧 | 累积 | `audioFramesDecoded` |
| `aBytesWrit` | 音频字节写入数 | 字节 | 累积 | `audioBytesWritten` |

**正常范围**：
- 三个值随播放时间线性增长；每 5 秒 `aFrmDec` 约增长 215（48kHz AAC，1024 samples/frame）
- `aBytesWrit` 约为 `aFrmDec × frame_bytes`（立体声 s16 为 4 字节/sample，即 4096 字节/帧）

**异常含义**：
- 增长停滞：音频解码线程阻塞、音频流丢失、或 `--no-audio` 模式开启
- 值为 0：无音频流或音频解码未启动

**数据来源**：`AudioWorker::run()` 每解码 10 帧更新一次统计；线程退出时最终写入一次。

**注意**：这三个字段为**累积值**（`load()`），不清零。对比连续两行可计算增量。

---

### 2.3 队列峰值

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `vQPeakMs` | 视频包队列峰值时长 | 毫秒 | 增量（峰值） | `videoQueuePeakMs` |
| `vQPeakPkt` | 视频包队列峰值包数 | 个 | 增量（峰值） | `videoQueuePeakPkts` |
| `aQPeakMs` | 音频队列峰值时长 | 毫秒 | 增量（峰值） | `audioQueuePeakMs` |

**正常范围**：
- `vQPeakMs`：正常模式 ≤ 200ms；低延迟模式（`--setpts-zero`）≤ 33ms
- `vQPeakPkt`：正常约 10-50 个包；低延迟模式通常 < 10 个
- `aQPeakMs`：≤ 100ms（正常模式）；≤ 66ms（低延迟模式）

**异常含义**：
- `vQPeakMs` 接近容量上限：网络接收快于解码消费，可能导致延迟累积
- `vQPeakPkt` 为 0 但 `vFrmDec` > 0：解码快于接收，属于正常；若同时 `vFrmDec` = 0，则流已中断
- 高 `vQPeakMs` + 高 `vFrmDrop`：解码/渲染瓶颈导致队列堆积

**数据来源**：
- `vQPeakMs` 和 `vQPeakPkt`：`PacketQueue::push()` 中维护内部 `m_peakDurationMs`/`m_peakSize`，通过 `drainPeak()` 读取并清零；同时 `recordQueueDepth()` 在不被 `drainPeak` 覆盖时跟踪 `PlayerStats` 级别的峰值
- `aQPeakMs`：**当前实现存在误导**——该字段实际存储的是 `m_frameQueue->count() * 33`（帧队列槽位占用 × 估计帧间隔），而非音频包队列的峰值时长。详见"已知问题"

---

### 2.4 延迟与跳帧

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `latAvgMs` | 最后延迟 | 毫秒 | 快照 | `lastLatenessUs / 1000` |
| `latMaxMs` | 最大延迟 | 毫秒 | 增量（峰值） | `maxLatenessUs / 1000` |
| `skipBurst` | 跳帧标记 | 布尔 | 增量 | `renderSkipBurst` |

**正常范围**：
- `latAvgMs`：命名为"Avg"但有误导性——实际是**上次渲染检查时的延迟快照**（上一帧的 overdue 值），不是区间平均值。正常 < 10ms
- `latMaxMs`：5 秒内最大延迟；正常 < 30ms。超过 50ms 表示渲染线程出现严重滞后
- `skipBurst`：0 = 本区间无单帧延迟丢帧；1 = 本区间至少发生一次单帧延迟丢帧

**延迟判定逻辑**（`RTSPlayer::videoRefresh()`）：
1. 计算 `actualDelaySec = targetTime - nowSec`
2. 若为负值（即帧已经"过期"），计算 overdue 微秒数
3. overdue 更新 `lastLatenessUs` 和 `maxLatenessUs`
4. 若 overdue > 50ms（`DROP_THRESHOLD_US`）且距上次丢帧 > 33ms（`MIN_DROP_INTERVAL_US`），则丢弃该帧并设置 `renderSkipBurst = 1`

**异常含义**：
- `latMaxMs` 持续增长：渲染管线过载（软件缩放瓶颈 / GPU 纹理上传慢 / 主线程被阻塞）
- `skipBurst = 1` 频繁出现：解码速率超出渲染能力，或 A/V 同步导致帧堆积

---

### 2.5 解码耗时

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `vDecSendMaxUs` | 最大送解码耗时 | 微秒 | 增量（峰值） | `decodeSendUsMax` |
| `vDecRecvMaxUs` | 最大收解码耗时 | 微秒 | 增量（峰值） | `decodeReceiveUsMax` |
| `vDecErr` | 解码错误次数 | 次 | 增量 | `decodeErrorCount` |

**正常范围**：
- `vDecSendMaxUs`：< 1000μs（1ms）。超过 50ms 触发 `LOG_WARN("send_packet slow")`
- `vDecRecvMaxUs`：< 5000μs（5ms）。超过 50ms 触发 `LOG_WARN("receive_frame slow")`
- `vDecErr`：始终为 0。任何非零值表示码流损坏或不支持的编码格式

**测量方式**：
- `vDecSendMaxUs`：`av_gettime_relative()` 包裹 `avcodec_send_packet()`，取区间峰值
- `vDecRecvMaxUs`：`av_gettime_relative()` 包裹 `avcodec_receive_frame()`，取区间峰值
- `vDecErr`：`sendErrs + recvErrs` 之和

**异常含义**：
- 单次 `vDecSendMaxUs` > 50ms：解码器内部缓冲满（可能是 B 帧重新排序、分辨率切换）
- `vDecRecvMaxUs` 持续偏高：CPU 瓶颈，或解码线程被抢占
- `vDecErr > 0`：码流损坏（网络丢包导致）、编码格式不兼容、或 FFmpeg 版本 bug

---

### 2.6 帧队列状态

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `fqFail` | 帧队列写入失败 | 次 | 增量 | `frameQueueWriteFailures` |
| `fqOverwrt` | 帧队列覆盖写入 | 次 | 增量 | `frameQueueOverwrites` |
| `fqPeakSlots` | 帧队列峰值占用 | 个 | 增量（峰值） | `frameQueuePeakSlots` |

**正常范围**：
- `fqFail`：始终为 0。表示写入的 AVFrame 数据为空、宽高 ≤ 0
- `fqOverwrt`：正常为 0；偶尔 1-2 次可接受。表示 24 个槽位全满，覆盖最旧帧
- `fqPeakSlots`：正常 ≤ 12（~400ms）；低延迟模式 ≤ 3

**帧队列容量**：24 槽位（`kSlotCount = 24`），约 800ms @ 30fps

**异常含义**：
- `fqFail > 0`：解码器产出损坏帧（width/height ≤ 0 或 data 指针为空）。需检查码流和 FFmpeg 版本
- `fqOverwrt` 频繁出现：渲染线程无法及时消费帧，形成积压。可能原因：渲染耗时过高、主线程阻塞、A/V 同步导致渲染等待
- `fqPeakSlots` 接近 24：帧队列接近满载，即将触发覆盖写入

**数据来源**：
- `fqFail`：`VideoFrameQueue::writeFrame()` 入口处的帧有效性校验
- `fqOverwrt`：`VideoFrameQueue::writeFrame()` 中检测到 `m_count >= kSlotCount` 时递增；同时递增 `framesDropped`
- `fqPeakSlots`：`RTSPlayer::videoRefresh()` 中每 30 帧提取一次当前 `m_frameQueue->count()` 并取峰值

---

### 2.7 音频环缓冲

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `aUnder` | 音频欠载次数 | 次 | 增量 | `audioUnderruns` |
| `aOver` | 音频过载次数 | 次 | 增量 | `audioOverruns` |
| `aRingFillB` | 环缓冲填充字节 | 字节 | 快照 | `audioRingFillBytes` |
| `aRingEmptyN` | 环缓冲读空次数 | 次 | 增量 | `audioRingReadEmpty` |
| `aRingBlockN` | 环缓冲写阻塞次数 | 次 | 增量 | `audioRingWriteBlocked` |

**正常范围**：
- `aUnder`：输出为 PCM 静音填充时为 0；偶尔有 1-3 次可能听到轻微爆音
- `aOver`：始终为 0（代码中从未递增，见"已知问题"）
- `aRingFillB`：正常模式下 4000-20000 字节；低延迟模式 2000-8000 字节。为 0 表示环缓冲已枯竭
- `aRingEmptyN`：正常为 0。与 `aUnder` 同步递增
- `aRingBlockN`：始终为 0（代码中从未递增写入），见"已知问题"

**环缓冲容量**：
- 正常模式：100ms（`kDefaultBufferMs = 100`），即 48000 × 4 × 0.1 = 19200 字节
- 低延迟模式：60ms（`kLowLatencyBufferMs = 60`），即 48000 × 4 × 0.06 = 11520 字节
- 区块数：32（`kMaxChunks`）

**异常含义**：
- `aUnder > 0`：音频环缓冲枯竭，SDL 音频回调读不到数据。原因：网络卡顿导致音频包延迟到达，音频解码速度不足，或解码线程被饿死
- `aRingFillB` 持续增大：音频写入快于硬件消费，环缓冲接近满——但实际上写满时是**静默覆盖旧数据**（不触发 `aOver` 递增）
- `aRingEmptyN` 增长：读空事件增多，即 SDL 回调调用 `read()` 时 `m_avail <= 0`

**数据来源**：
- `aUnder`：`AudioRingBuffer::read()` 的两个重载中，当 `m_avail <= 0` 时递增
- `aRingFillB`、`aRingEmptyN`：`RTSPlayer::videoRefresh()` 中每 30 帧通过 `snapshotRingCounters()` 同步采样

---

### 2.8 渲染节奏

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `rndIntvAvgMs` | 平均渲染间隔 | 毫秒 | 增量（区间平均） | `paintIntervalSumUs / pc / 1000` |
| `rndIntvMaxMs` | 最大渲染间隔 | 毫秒 | 增量（峰值） | `paintIntervalMaxUs / 1000` |
| `rndLatAvgMs` | 平均渲染延迟 | 毫秒 | 增量（区间平均） | `paintLatencySumUs / pl / 1000` |
| `rndLatMaxMs` | 最大渲染延迟 | 毫秒 | 增量（峰值） | `paintLatencyMaxUs / 1000` |

**正常范围**：
- `rndIntvAvgMs`：30fps 约为 33ms；60fps 约为 16ms。反映实际渲染调用间隔
- `rndIntvMaxMs`：正常 < 50ms。过长表示主线程在周期性任务（日志、CSV 写入）中阻塞
- `rndLatAvgMs`：正常 < 5ms（swscale + SDL_UpdateYUVTexture + SDL_RenderCopy）
- `rndLatMaxMs`：正常 < 20ms。超过 50ms 表示渲染管线瓶颈

**测量方式**：
- **渲染间隔**（paintInterval）：`RTSPlayer::videoRefresh()` 中 `av_gettime_relative()` 包裹两次渲染调用之间的时间差
- **渲染延迟**（paintLatency）：`RTSPlayer::videoRefresh()` 中 `displayFrame()` 前后的 `av_gettime_relative()` 差值

**异常含义**：
- `rndLatMaxMs` > 50ms：软件渲染瓶颈（swscale 颜色空间转换在高分辨率下开销大）。可能需考虑 GPU 加速（如 SDL_RenderCopy 硬件加速）
- `rndIntvMaxMs` 远超帧间隔：主线程被阻塞（日志 I/O、CSV 写入、SDL 事件处理卡顿）
- `rndLatAvgMs` ≈ `rndLatMaxMs` → 渲染延迟稳定；差异大 → 存在周期性尖刺

---

### 2.9 重连统计

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `reconn` | 重连次数 | 次 | 累积 | `reconnectCount` |
| `reconMs` | 重连总耗时 | 毫秒 | 累积 | `totalReconnectMs` |

**正常范围**：
- `reconn`：稳定网络下始终为 0
- `reconMs`：正常为 0

**异常含义**：
- `reconn > 0`：流已断开并成功重连。检查网络状况和推流端稳定性
- `reconMs` 较大（如 > 5000ms）：网络恢复缓慢或推流端重启耗时较长

**数据来源**：
- `reconnectCount`：`StreamLifecycleManager::doReconnect()` 成功后递增
- `totalReconnectMs`：从 `reconnectStartUs`（流错误检测时间）到 `doReconnect()` 成功的耗时累加

---

### 2.10 首帧时间戳

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `v1stDecUs` | 首帧视频解码时间 | 微秒 | 一次性 | `videoFirstDecodeUs` |
| `v1stRendUs` | 首帧视频渲染时间 | 微秒 | 一次性 | `videoFirstRenderUs` |
| `a1stDecUs` | 首帧音频解码时间 | 微秒 | 一次性 | `audioFirstDecodeUs` |
| `a1stPlayUs` | 首帧音频播放时间 | 微秒 | 一次性 | `audioFirstPlayUs` |

**数据性质**：所有首帧时间戳使用 `compare_exchange_strong(expectedZero, av_gettime_relative())` 确保仅设置一次，后续保持不变。

**正常范围**：
- `v1stDecUs`：启动后 100-3000ms（取决于网络 RTT、SDP 协商、IDR 到达时间）
- `a1stDecUs`：通常晚于视频首帧解码 50-500ms
- `a1stPlayUs`：通常晚于 `a1stDecUs` 20-100ms（环缓冲积累到 SDL 回调触发所需的时间）

**`v1stRendUs` 始终为 0**：源码中 `videoFirstRenderUs` 声明但从未被写入。此字段无意义，见"已知问题"。

---

### 2.11 帧 ID

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `frameId` | 当前渲染帧 ID | — | 快照 | `frameId` |

单调递增的帧计数器，初始值 1。每次 `RTSPlayer::videoRefresh()` 成功渲染一帧后递增。

**用途**：用于关联日志时间线——日志中的 `Render #N` 与此字段对应，帮助定位特定帧的渲染问题。

---

### 2.12 卡顿/同步诊断

| CSV 列名 | 中文名 | 单位 | 数据性质 | 源码字段 |
|----------|--------|------|----------|----------|
| `vPopTO` | 视频解码 Pop 超时事件 | 次 | 增量 | `videoPopTimeouts` |
| `catDrop` | 追赶丢帧数 | 帧 | 增量 | `catchUpDrops` |
| `vStall` | 视频卡顿事件 | 次 | 增量 | `videoStallCount` |
| `audDiffMs` | 最后帧-音频差 | 毫秒 | 快照 | `frameAudDiffMs` |
| `clkDiffMs` | 最后时钟差 | 毫秒 | 快照 | `clockDiffMs` |

**正常范围**：
- `vPopTO`：始终为 0。非零表示严重的网络/解码中断
- `catDrop`：正常 ≤ 5。超过 20 表示频繁追赶（视频解码超前音频）
- `vStall`：始终为 0。非零表示网络断流或推流中断
- `audDiffMs`：正常 -40 ~ +40ms。正值 = 视频超前音频；负值 = 视频落后音频
- `clkDiffMs`：正常 -20 ~ +20ms。反映视频时钟与音频时钟的差值（含漂移）

**详细机制**：

**`vPopTO`（videoPopTimeouts）**：视频解码线程中，当 10 次连续 `pop` 超时（每次 100ms，即连续 1 秒无数据包）时递增。这是一个**延迟告警**事件。

**`vStall`（videoStallCount）**：当连续 10 次 pop 超时结束（即数据恢复到达）时递增，同时记录 stall 持续时长。表示一次完整的"无数据 → 恢复"周期。

**`catDrop`（catchUpDrops）**：在两种场景中递增：
1. A/V 同步追赶：`avDiff < -maxAudioLagForQueue` 时，视频落后音频超过阈值，从帧队列丢弃旧帧追赶
2. Setpts-zero 模式：仅保留最新一帧渲染，丢弃其余帧

**`audDiffMs`（frameAudDiffMs）**：当前帧 PTS 减去音频时钟（`framePtsSec - audioClockNow`），在每次 `videoRefresh()` 的时钟计算阶段写入。

**`clkDiffMs`（clockDiffMs）**：视频时钟 PTS 减去音频时钟 PTS（`videoClockNow - audioClockNow`），在每次 `videoRefresh()` 的时钟计算阶段写入。

**异常含义**：
- `vPopTO > 0` + `vStall > 0`：网络抖动导致数据包到达中断。检查网络质量、WiFi 信号
- `catDrop` 持续高位：视频解码速率远超渲染消费，或 A/V 同步阈值过小。低延迟模式下更易触发
- `|audDiffMs| > 200`：A/V 同步严重失调。正值过大 = 视频领先太多；负值过大 = 视频严重落后
- `|clkDiffMs| > 100`：视频/音频时钟漂移过大，可能由 SDL 音频设备延迟或推流端 PTS 异常引起

---

## 3. 已知问题

### 3.1 `v1stRendUs` —— 声明但从未写入

`PlayerStats::videoFirstRenderUs` 在头文件中声明，在 CSV 输出中读取，但在整个源码中**没有任何写入点**。该字段始终为 0，无参考价值。

- 文件：`src/PlayerStats.h:71`（声明）
- CSV 输出：`src/PlayerStats.cpp:100`（读取）

### 3.2 `aOver`（audioOverruns）—— 声明但从未递增

`PlayerStats::audioOverruns` 在头文件中声明，在 CSV 中输出，但**所有写入点仅操作 `audioUnderruns`**。环缓冲写满时，`AudioRingBuffer::write()` 静默丢弃最旧数据块，不递增任何 overrun 计数器。

- 文件：`src/PlayerStats.h:24`（声明）
- 写满逻辑：`src/AudioRingBuffer.cpp:26-36`（静默覆盖，无 overrun 计数）

### 3.3 `queueVideoDurationMs` —— 死代码

`PlayerStats::queueVideoDurationMs` 声明但**从未被读写**（声明之外无任何引用）。属于早期设计的残留字段。

- 文件：`src/PlayerStats.h:16`

### 3.4 `lastCommitUs` —— 死代码

`PlayerStats::lastCommitUs` 声明但**从未被读写**（声明之外无任何引用）。属于早期设计的残留字段，且不输出到 CSV。

- 文件：`src/PlayerStats.h:76`

### 3.5 `aQPeakMs` 含义偏差

`audioQueuePeakMs` 字段从名称看应为音频包队列峰值时长，但实际填充的是 `m_frameQueue->count() * 33`（帧队列槽位占用 × 估计帧间隔 33ms）。该字段实际反映的是帧队列占用近似值，而非音频队列时长。

- 数据来源：`src/RTSPlayer.cpp:382` → `PlayerStats::recordQueueDepth()`

### 3.6 `aRingBlockN`（audioRingWriteBlocked）—— 声明但从未递增

`m_writeBlockCount` 在 `AudioRingBuffer.h:75` 声明，在 CSV 中输出，但**所有写入点均未递增该计数器**。`write()` 方法在锁外通知 `m_cv.notify_one()` 但无阻塞等待，故没有真正的"写阻塞"场景。

- 文件：`src/AudioRingBuffer.h:75`（声明）

### 3.7 `elapsed_s` 命名误导

CSV 头列为 `elapsed_s`，暗示自启动后经过的秒数，但实际存储的是 `time(nullptr)` Unix 时间戳。相邻两行的差值约 5 秒（即为实际经过时间）。

- 文件：`src/PlayerStats.cpp:111`

### 3.8 `latAvgMs` 命名误导

CSV 头列为 `latAvgMs`，暗示区间平均延迟，但实际上存储的是 `lastLatenessUs / 1000`——**上一帧的延迟快照**（不清零，持续覆盖）。该值是间距最近的单帧 overdue 值，而非 5 秒区间的平均值。

- 文件：`src/PlayerStats.cpp:65`

### 3.9 `skipBurst` 实现缺陷

`renderSkipBurst` 的写入逻辑中，`burstCount` 恒为 1（而非累加），导致该字段退化为布尔标志（0 = 无丢帧事件，1 = 有丢帧事件），丢失了并发丢帧次数信息。

- 文件：`src/RTSPlayer.cpp:312`（`burstCount = 1` 而非 `curBurst + 1`）
- 清零函数 `recordSkipBurstEnd()` 声明但从未被调用：`src/PlayerStats.cpp:181`

---

## 4. 典型异常场景速查表

| 症状 | 关键字段表现 | 可能原因 | 排查方向 |
|------|-------------|----------|----------|
| 高丢帧率 + 高 `catDrop` | `vDropRate_pct > 5%`, `catDrop > 20` | 视频解码超前音频，A/V 同步频繁追赶 | 检查帧 PTS 间隔；确认推流端 PTS 是否正确；降低分辨率 |
| 高丢帧率 + 高 `latMaxMs` | `vDropRate_pct > 5%`, `latMaxMs > 50ms` | 渲染线程过载，帧来不及显示 | 检查 CPU 占用；检查 `rndLatMaxMs`；确认 swscale 分辨率 |
| 高丢帧率 + 高 `fqOverwrt` | `vDropRate_pct > 5%`, `fqOverwrt > 5` | 帧队列积压，消费者跟不上生产者 | 检查 `rndIntvMaxMs`；确认主循环未被阻塞 |
| 高 `vStall` | `vStall > 0`, `vPopTO > 0` | 网络抖动 / 推流中断 | 检查网络质量（ping/丢包率）；确认推流端运行状态 |
| `reconn > 0` | `reconn > 0`, `reconMs` 记录耗时 | 流断连并重连 | 检查 `reconMs` 判断恢复耗时；检查网络稳定性 |
| 高 `aUnder` | `aUnder > 10`, `aRingEmptyN` 同步增长 | 音频环缓冲枯竭 | 检查音频解码线程状态；检查 `aPktIn` 是否增长；网络卡顿导致音频包晚到 |
| 高 `rndLatMaxMs` | `rndLatMaxMs > 50ms` | 渲染管线瓶颈 | 检查 swscale 缩放分辨率；确认 SDL 纹理格式；GPU 驱动 |
| 高 `vDecErr` | `vDecErr > 0` | 解码器异常 | 检查码流是否损坏；确认编码格式兼容性（H.264/H.265）；检查 FFmpeg 版本 |
| 音频静音 | `aUnder` 持续增长, `aRingFillB = 0` | 环缓冲完全枯竭 | 检查音频流是否存在；确认推流端有音频轨道 |
| A/V 不同步 | `|audDiffMs| > 200` | 时钟漂移 | 检查推流端 PTS；确认 SDL 音频设备采样率匹配 |
| 启动慢 | `v1stDecUs > 5000000` | 首帧解码超过 5 秒 | 检查网络 RTT；确认推流端 IDR 间隔；检查 SDP 协商 |
