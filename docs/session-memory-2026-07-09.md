---
type: project-session-memory
project: RTSP-Proj
created_at: 2026-07-09T11:15:26+08:00
status: diagnosis-complete-modification-pending
---

# RTSP-Proj 集成测试会话记忆

## 当前背景

- `RTSP-Pusher` 在 Windows 11 x64 设备运行，使用 QSV、ddagrab、1920×1080、30 fps、12 Mbps、TCP。
- `RTSP-Player` 在 Windows 10 x86 设备运行，使用软件解码、TCP、`--setpts-zero`。
- 两端由各自服务通过 `QProcess` 启动。
- 集成测试现象是端到端延迟不稳定，拉流端间歇性花屏。
- 本次分析使用 `D:\tmp` 下的推流、拉流、服务日志和 CSV 指标。

## 已完成工作

1. 完整梳理播放器和推流器的目录、构建方式、线程模型、数据流、状态机、重连及队列策略。
2. 生成项目逻辑文档：`docs/project-overview-logic.md`。
3. 对 2026-07-09 的约 2796 秒集成测试日志建立双端时间轴。
4. 统计推流端 mux 阻塞、编码队列深度、音画 PTS 偏移。
5. 统计拉流端解码耗时、追赶次数、丢帧、音频欠载和视频压缩包丢弃。
6. 将日志现象与当前源码逐项交叉核对。

## 核心结论

### 花屏直接原因

`RTSP-Player/src/PacketQueue.cpp` 在视频包队列超过容量、等待 100 ms 仍未腾空后，会删除队列中的单个 H.264 非关键包，然后继续把后续压缩包交给解码器。

本次日志记录到 9 次：

- 09:12:16.728
- 09:12:17.093
- 09:12:17.217
- 09:12:17.865
- 09:12:18.533
- 09:12:18.683
- 09:12:18.882
- 09:12:22.533
- 09:13:21.715

非关键 H.264 帧仍可能是后续帧的参考帧。删除单包后继续解码会破坏参考链，表现为马赛克或局部花屏，直到下一个 IDR。

### 延迟不稳定的主要原因

推流端 `av_interleaved_write_frame()` 存在持续阻塞：

- 559 个统计窗口中，458 个窗口峰值超过 100 ms。
- 159 个窗口峰值超过 200 ms。
- 平均窗口峰值约 192 ms。
- P95 约 478 ms。
- 最大值 1.228 秒。
- 编码包队列有 83 个窗口达到容量上限 60。

该现象形成“mux 阻塞、队列积压、突发发送、拉流端追赶”的循环。

### 拉流设备性能不足

- x86 构建的 `--hwaccel auto` 会禁用 DXVA2，实际使用软件解码。
- 视频解码 `send_packet` 窗口峰值平均约 56.6 ms，最大 198 ms。
- 358/559 个窗口峰值超过 50 ms。
- 最终解码 75703 帧、渲染 73657 帧、丢弃 2046 帧，累计约 2.7%。
- 快速追赶模式进入 1508 次。
- 渲染间隔最大 2.056 秒。

1080p30、12 Mbps、x86 软件解码与 67 ms 低延迟视频包队列组合过于激进。

### 其他问题

- 推流音频 PTS 平均落后视频约 489 ms，结束时约 547 ms。
- 首次音频采集比视频晚约 335 ms，但音视频分别从 PTS 0 开始。
- `QProcess` 正常停止被服务误判为崩溃；推流退出码为 62097。
- 拉流服务等待 3 秒后强制终止播放器。
- 推流命令选择 `--audio-device-index 0`，实际是麦克风；立体声混音是设备 1。
- `D:\tmp\rtsp_pusher_stats.csv` 内容属于 2026-07-08，不是本次 2026-07-09 测试。

## 已确定的花屏修改方案

### 拉流端必改

修改：

- `RTSP-Player/src/PacketQueue.h`
- `RTSP-Player/src/PacketQueue.cpp`
- `RTSP-Player/src/StreamLifecycleManager.cpp`
- `RTSP-Player/src/VideoDecodeThread.cpp`

策略：

1. 视频队列增加 `keyframeAware` 模式。
2. 视频队列溢出时清空整个旧 GOP，不再删除单个非关键包后继续解码。
3. 进入 `waitingForKeyframe` 状态，丢弃后续所有非关键视频包。
4. 收到下一个带 `AV_PKT_FLAG_KEY` 的包后恢复入队。
5. 队列设置 `discontinuity` 标志。
6. `VideoDecodeThread` 在发送恢复关键帧之前调用 `avcodec_flush_buffers()`。
7. `flush()` 同时复位 `waitingForKeyframe` 和 `discontinuity`。

预期行为是将“花屏”转化为最长一个 GOP 的短暂冻结。当前 GOP 为 30、帧率为 30 fps，理论最长约 1 秒。

### 推流端检查

`RTSP-Pusher/src/EncodedPacketQueue.cpp` 工作区已有“溢出后等待视频关键帧”的未提交修改，需要确认实际部署的 `RTSP-Pusher.exe` 包含该版本。

当前代码在关键帧成功重新入队后仍返回 `false`，会把已成功入队的关键帧误计为丢帧，应改为返回 `true`。

## 建议验证顺序

1. 实施播放器关键帧恢复和解码器 flush 修改。
2. 保持其他配置不变运行 30 分钟，确认日志不再出现 `PacketQueue[video] dropping non-key frame`。
3. 验证只出现以下恢复日志：
   - `overflow: waiting for keyframe`
   - `resumed at keyframe`
   - `stream discontinuity, flushing decoder`
4. 如果仍频繁溢出，移除播放器 `--setpts-zero` 做 A/B 测试。
5. 将视频包队列由 67 ms 提高到 200 ms 做第二组 A/B 测试。
6. 将推流码率由 12 Mbps 降到 6–8 Mbps，观察 mux 写阻塞和队列顶满次数。
7. 优先测试 x64 拉流硬解；无法更换时测试 720p/25 fps。

## 未验证项

- 未取得花屏发生时的录像或人工标记时间，尚未将画面与 9 次视频包丢弃逐帧对齐。
- 未检查 RTSP Server、交换机和网络吞吐指标。
- 未核对测试设备部署二进制与工作区构建产物的哈希。
- 尚未执行修改后的双机 A/B 测试。

## 文件状态

- 本次会话未修改播放器或推流器源码。
- 新增：
  - `docs/project-overview-logic.md`
  - `docs/session-memory-2026-07-09.md`
- `RTSP-Pusher` 原本已有用户未提交源码修改和构建产物变化，必须保留。

## 恢复时的第一步

从 `RTSP-Player/src/PacketQueue.h` 开始实现关键帧感知的溢出恢复状态，然后修改 `PacketQueue.cpp`、`StreamLifecycleManager.cpp` 和 `VideoDecodeThread.cpp`，编译 x86 播放器并执行 30 分钟双机验证。
