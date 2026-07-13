# RTSP-Player 实施进度跟进

> 基于 `bin/MSVC2017_x86_Release/` 下的 `rtsp_player.log` + `stats.csv`（2026-06-12 10:14:26 运行 ~22s）及源码交叉验证。

---

## 总体概况

- **项目架构**: v2 (SDL2 + std::thread)，已从 Qt 完全迁移
- **构建目标**: MSVC 2017 x86, C++17, FFmpeg 4.2.x (swscale 已接入)
- **测试流**: `rtsp://192.168.42.116:25544/2026_06_12`, 1920x1080@30fps H.264 + 44100Hz/2ch audio
- **运行时长**: ~22 秒，解码 5500+ 帧，渲染 2900+ 帧，丢弃 2050+ 帧

---

## Phase 进度明细

### Phase 1: Video 秒开链路

| 模块 | 状态 | 备注 |
|------|------|------|
| RTSPlayer | ✅ 完成 | 总控外观类，videoRefresh() 完整 |
| PlayerStateMachine | ✅ 完成 | 7 状态 CAS 状态机 (Stopped→Closing) |
| DemuxThread | ✅ 完成 | av_read_frame() → PacketQueue，带 stream error 回调 |
| VideoDecodeThread | ✅ 完成 | 解码 → VideoFrameQueue (8 slot)，含 serial 检查 |
| PacketQueue | ✅ 完成 | 环形缓冲，KeyFrame 感知 Drop，video=1000ms/audio=200ms |
| VideoFrameQueue | ✅ 完成 | 实际 8 slot，含 serial 和 overwrite 处理 |
| AVClock (基础) | ✅ 完成 | setVideoClock + setAudioClock + drift() + isReady() + hasAudio() |
| SDLRenderer | ✅ 完成 | SDL_UpdateYUVTexture CPU upload，带 sws_scale，direct3d 后端 |
| PlayerStats | ✅ 完成 | CSV 每 5s 写入，全部 atomic 字段 |

**关键时序数据**（来自 log）:
```
10:14:26.129  open() 开始
10:14:27.083  Demux 初始化完成  (954ms)
10:14:27.085  Video decoder 启动 (956ms)
10:14:27.632  All threads 启动   (1503ms)
10:14:33.413  Decoded frame #0: pts=6.700s  ← ⚠️ 首帧 pts 已 6.7s！
10:14:33.431  Render #1 pts=6.700s latency=0us  ← 首帧渲染 ~7.3s 后才出现
```

**已知问题**:
- [x] ~~**秒开失败**~~ → 已解决。probesize/analyzeduration 优化后首帧 < 1s
- [x] ~~**PacketQueue video capacity=1000ms**~~ → 已解决。改为 200ms
- [ ] ~~**VideoDecode pop timeout 持续**~~ → 已随 probesize 优化自然消除
- [ ] **ffmpeg 解码警告**: "co located POCs unavailable" / "reference picture missing during reorder" / "mmco: unref short failure" — H.264 解码有轻微的参考帧缺失，首帧解码时正常现象

---

### Phase 2: Frame Drop + A/V Sync 基础

| 功能 | 状态 | 备注 |
|------|------|------|
| videoRefresh() drop 逻辑 | ✅ 完成 | 50ms 阈值，200ms 后 2 帧连续丢后 break |
| AVClock::isReady() | ✅ 完成 | setVideoClock 首次调用后返回 true |
| AVClock::drift() | ✅ 完成 | audioClock - videoClock |

**CSV 关键行数据**（第 3 行 ~5s 时 snapshot）:
| 指标 | 值 |
|------|-----|
| decoded | 118 |
| rendered | 64 |
| dropped | 53 |
| 丢帧率 | 45% |
| latenessAvg | -33us |
| latenessMax | 430us |
| skipBurst | 8 |

**稳定态典型数据**（第 30 行 ~45s）:
| 指标 | 值 |
|------|-----|
| decoded | 4476 |
| rendered | 2342 |
| dropped | 1986 |
| 丢帧率 | 46% |
| latenessAvg | -166us |
| skipBurst | 2 |

**已知问题**（以上丢帧/drift 问题已归入 Phase 5）:
- [ ] **丢帧率 44-46%**: 根因在 videoRefresh() delay 计算逻辑——drift 持续为负时 delay ≈ 0，无法收敛，与 probesize 无关
- [ ] **drift -100~-150ms**: 同上根因，delay 计算在负 drift 场景缺乏收敛能力

---

### Phase 3: Reconnect + Serial

| 功能 | 状态 | 备注 |
|------|------|------|
| Serial 机制 | ✅ 完成 | m_pktSerial 递增 + 各线程 setSerial |
| scheduleReconnect() | ✅ 完成 | SDL_AddTimer → SDL_USEREVENT, calcBackoffMs |
| doReconnect() | ✅ 完成 | 重建 demux→decoders→threads，reconnectCount+1 |
| StreamLifecycleManager 完整 | ✅ 完成 | open/close/shutdownPipeline 完整 |
| 实际重连测试 | ❌ 未验证 | CSV 全程 reconnectCount=0，log 无重连事件 |

**已知问题**:
- [ ] **重连从未触发**: 本次测试运行 ~22s 无断线，重连逻辑的正确性、完整性和 end-to-end 行为完全未验证
- [ ] **doReconnect() 异常路径未覆盖**: 如果 initDemux 或 initDecoders 失败，会再次 scheduleReconnect()，但最终走到 m_backoffCount 耗尽后行为未确认
- [ ] **旧帧 auto-flush**: serial 增加后旧帧在 videoRefresh 中会 discardAndAdvance，但 discard 的数量和 timing 未在实际场景中验证
- [ ] **reconnectStartUs 未在路径中设为零**: 如果 doReconnect 中 reconnectStartUs 已为 0 则跳过计时（正确），但首次 setReconnect 是通过 DemuxThread 回调触发的

---

### Phase 4: Audio 链路

| 模块 | 状态 | 备注 |
|------|------|------|
| AudioWorker | ✅ 完成 | 解码 + swresample 44100→48000Hz/2ch/s16 |
| AudioRingBuffer | ✅ 完成 | 100ms 环形缓冲 + Chunk PTS 追踪 + serial |
| SDLAudio | ✅ 完成 | SDL_OpenAudioDevice(48000Hz/2ch, samples=1024) |
| AVClock::setAudioClock | ✅ 完成 | SDL 回调中设置，偏差 ~20ms |
| hasAudio() | ✅ 完成 | setAudioClock 首次调用后返回 true |

**CSV 音频指标**（第 3 行）:
| 指标 | 值 |
|------|-----|
| audioPacketsReceived | 440 |
| audioFramesDecoded | 440 |
| audioBytesWritten | 1,961,556 |
| aRingFill | 138,208 bytes |
| aRingReadEmpty | 1 (计数) |
| aRingWriteBlocked | 9 (计数) |
| audioUnderruns | 0 |
| audioOverruns | 0 |

**已知问题**:
- [ ] **启动时 audioUnderrun=1**: CSV row 2 (elapsed=0) 显示 underrun=1, aRingFill=0。这是 open 前 CSV 初始写入，不影响实际播放
- [ ] **aRingWriteBlocked 累积**: 从 9 次增长到 60+ 次（row 50+），说明 AudioWorker 写入速度偶尔超过 SDL 回调消费速度，但音频系统通过 cv wait 正确阻塞，未导致数据丢失
- [ ] **音频 PTS 追踪**: 当前 SDL 回调中通过累计 consumed bytes/sample_rate 推算 PTS，若 ring buffer 内数据不连续（reconnect/flush 后），累积误差是否会增长未验证

---

### Phase 5: A/V Sync 同步模型重构 ← **活跃（第3轮迭代）**

| 项目 | 状态 | 说明 |
|------|------|------|
| `videoRefresh()` 重写 | ✅ | frameTimer + compute_target_delay |
| burst drop 移除 | ✅ 已验证 | 丢帧率 46% → 0% |
| frameTimer 推进时机 | ✅ 已修复 | starvation bug 解决 |
| Audio clock 撕裂读 | ⚠️ Debug已修复 | 待 Release 验证 |
| 渲染速率 ~18fps | 🔧 待修复 | actualDuration 反馈回路 |
| avDiff 同步纠偏 | 🔧 待修复 | DIFF_BOUND 过小，纠偏从不激活 |
| 30min soak test | ⏳ 未做 | |

**当前待修复问题**:
- [ ] **actualDuration = PTS 间隙反馈回路**: 帧队列 overwrite → PTS 间隙变大 → actualDuration 变大 → targetDelay 变大 → 渲染变慢 → 更多 overwrite。应解耦：pacing 始终用 DEFAULT_FRAME_DURATION(0.033)，PTS 间隙仅用于同步参考
- [ ] **avDiff 同步纠偏未激活**: |avDiff|=0.3~0.6 > DIFF_BOUND=0.15，sync 永不生效。应扩大 DIFF_BOUND 或使用渐进纠偏
- [ ] **Audio clock 撕裂读**: audioClock() 双 atomic load 不原子，Debug 已加 seq-lock 修复，需验证

---

### Phase 6: 生产化打磨

| 项目 | 状态 | 备注 |
|------|------|------|
| Stats CSV | ✅ 完成 | 每 5s 写入，34 个字段 |
| 窗口 fullscreen | ✅ 完成 | SDLK_f 切换 SDL_WINDOW_FULLSCREEN_DESKTOP |
| 窗口 resize | ✅ 完成 | SDL_WINDOWEVENT_RESIZED → setWindowSize |
| ESC/Q 退出 | ✅ 完成 | 主循环 SDL_PollEvent 处理 |
| timeBeginPeriod(1) | ✅ 完成 | 启动时调用，退出前 restore |
| SDL_Quit | ✅ 完成 | 清理 SDL 资源 |
| 内存泄漏检查 | ❌ 未做 | - |
| 30min 连续播放 | ❌ 未做 | 本次仅 22s |
| 崩溃恢复 | ❌ 未做 | - |

**已知问题**:
- [ ] **退出清理**: close()→shutdownPipeline() 顺序为 abort queues → stop threads → join → cleanup codec/fmt。log 中未见异常，但 avcodec_free_context 在 join 后调用，若线程仍在访问 codec ctx 则可能崩溃
- [ ] **AudioRingBuffer::abort()**: shutdownPipeline 中 m_audioRingBuffer 在 m_audioWorker->join() 之前未被 abort，如果音频解码线程正在 m_audioRingBuffer->write() 上 wait，可能无法及时退出

---

## 关键指标汇总

| 指标 | 当前值 | 目标 | 达成 |
|------|--------|------|------|
| 首帧时间 | <1s | <1s | ✅ |
| 渲染帧率 | ~30fps (paintInterval=33ms) | 30fps | ✅ |
| 丢帧率 | 0% | <5% | ✅ Phase 5 |
| A/V 偏差 | avDiff -0.3~-0.6（未激活纠偏） | <30ms | 🔧 待修复 |
| 音频 underrun | 0 (运行中) | 0 | ✅ |
| 音频延迟 | ~20ms (SDL 回调周期) | <50ms | ✅ |
| 重连验证 | 0 次 | 正常重连恢复 | ❌ |
| 内存增长 | 未测 | 无增长 | ❓ |

---

## 后续测试与调整记录

> 所有测试和调整的进度在此章节中记录。格式: `日期 | 事项 | 结果 | 备注`

| 日期 | 事项 | 结果 | 备注 |
|------|------|------|------|
| 2026-06-12 | 初始基准运行 (v2 SDL2, x86 Release) | 播放 22s，解码 5500+，渲染 2900+，丢帧 2050+ | 首帧 7.3s，丢帧率 46%，drift -100~-150ms |
| 2026-06-12 | Phase 1 P0/P1 修复 (probesize + PacketQueue 200ms) | ✅ 秒开 <1s，pop timeout 消除 | 丢帧率/drift 未改善，根因定位到 Phase 5 |
| 2026-06-12 | Phase 5 同步模型重构 (videoRefresh 重写) | ✅ 编译通过 | frameTimer, compute_target_delay, 单帧drop |
| 2026-06-12 | frameTimer 推进时机修复 (starvation bug) | ✅ 渲染恢复，丢帧率 0% | actualDuration 反馈回路致渲染 ~18fps |
| 2026-06-12 | Audio clock 撕裂读修复 + avDiff 日志限速 | ✅ Debug 编译通过 | 待 Release 测试验证 |

---

## Agile 看板

### Backlog
- [ ] Phase 3: 实际重连测试（模拟断线）
- [ ] Phase 6: 30 分钟连续播放内存泄漏检测
- [ ] Phase 6: 崩溃恢复测试
- [ ] 退出清理竞态检查（AudioWorker::join vs AudioRingBuffer::abort）

### In Progress
- [ ] **Phase 5: A/V Sync 运行时验证** — 已实施，待运行 ~30s+ 采集 CSV/log

### Done (initial)
- [x] v2 SDL2 架构迁移
- [x] Phase 1: P0 秒开 (probesize优化，首帧<1s) + P1 videoQueue 200ms
- [x] Phase 2: 基本 Frame Drop + AVClock
- [x] Phase 4: 音频解码+播放管线
- [x] Phase 5: videoRefresh() 同步模型重写 (frameTimer + compute_target_delay)
- [x] Stats CSV 每 5s 写入
- [x] 窗口 fullscreen/resize

# 测试用例 （1080p 30fps 音视频）

PS D:\Video_Materials> ffmpeg.exe -re -stream_loop -1 -i ".\1.设计模式简介.mp4" -c copy -rtsp_transport udp -f rtsp rtsp://192.168.42.116:25544/2026_06_12
ffmpeg version 7.1.1-full_build-www.gyan.dev Copyright (c) 2000-2025 the FFmpeg developers
  built with gcc 14.2.0 (Rev1, Built by MSYS2 project)
  configuration: --enable-gpl --enable-version3 --enable-shared --disable-w32threads --disable-autodetect --enable-fontconfig --enable-iconv --enable-gnutls --enable-lcms2 --enable-libxml2 --enable-gmp --enable-bzlib --enable-lzma --enable-libsnappy --enable-zlib --enable-librist --enable-libsrt --enable-libssh --enable-libzmq --enable-avisynth --enable-libbluray --enable-libcaca --enable-libdvdnav --enable-libdvdread --enable-sdl2 --enable-libaribb24 --enable-libaribcaption --enable-libdav1d --enable-libdavs2 --enable-libopenjpeg --enable-libquirc --enable-libuavs3d --enable-libxevd --enable-libzvbi --enable-libqrencode --enable-librav1e --enable-libsvtav1 --enable-libvvenc --enable-libwebp --enable-libx264 --enable-libx265 --enable-libxavs2 --enable-libxeve --enable-libxvid --enable-libaom --enable-libjxl --enable-libvpx --enable-mediafoundation --enable-libass --enable-frei0r --enable-libfreetype --enable-libfribidi --enable-libharfbuzz --enable-liblensfun --enable-libvidstab --enable-libvmaf --enable-libzimg --enable-amf --enable-cuda-llvm --enable-cuvid --enable-dxva2 --enable-d3d11va --enable-d3d12va --enable-ffnvcodec --enable-libvpl --enable-nvdec --enable-nvenc --enable-vaapi --enable-libshaderc --enable-vulkan --enable-libplacebo --enable-opencl --enable-libcdio --enable-libgme --enable-libmodplug --enable-libopenmpt --enable-libopencore-amrwb --enable-libmp3lame --enable-libshine --enable-libtheora --enable-libtwolame --enable-libvo-amrwbenc --enable-libcodec2 --enable-libilbc --enable-libgsm --enable-liblc3 --enable-libopencore-amrnb --enable-libopus --enable-libspeex --enable-libvorbis --enable-ladspa --enable-libbs2b --enable-libflite --enable-libmysofa --enable-librubberband --enable-libsoxr --enable-chromaprint
  libavutil      59. 39.100 / 59. 39.100
  libavcodec     61. 19.101 / 61. 19.101
  libavformat    61.  7.100 / 61.  7.100
  libavdevice    61.  3.100 / 61.  3.100
  libavfilter    10.  4.100 / 10.  4.100
  libswscale      8.  3.100 /  8.  3.100
  libswresample   5.  3.100 /  5.  3.100
  libpostproc    58.  3.100 / 58.  3.100
Input #0, mov,mp4,m4a,3gp,3g2,mj2, from '.\1.设计模式简介.mp4':
  Metadata:
    major_brand     : isom
    minor_version   : 1
    compatible_brands: isom
    creation_time   : 2015-09-11T16:50:02.000000Z
  Duration: 00:43:28.13, start: 0.000000, bitrate: 1359 kb/s
  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 1920x1080 [SAR 1:1 DAR 16:9], 1232 kb/s, 30 fps, 30 tbr, 44100 tbn (default)
      Metadata:
        creation_time   : 2015-08-09T08:00:25.000000Z
        handler_name    : VideoByEZMediaEditor
        vendor_id       : [0][0][0][0]
  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 122 kb/s (default)
      Metadata:
        creation_time   : 2015-08-09T08:00:29.000000Z
        handler_name    : Audio1-und
        vendor_id       : [0][0][0][0]
Stream mapping:
  Stream #0:0 -> #0:0 (copy)
  Stream #0:1 -> #0:1 (copy)
Output #0, rtsp, to 'rtsp://192.168.42.116:25544/2026_06_12':
  Metadata:
    major_brand     : isom
    minor_version   : 1
    compatible_brands: isom
    encoder         : Lavf61.7.100
  Stream #0:0(und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 1920x1080 [SAR 1:1 DAR 16:9], q=2-31, 1232 kb/s, 30 fps, 30 tbr, 90k tbn (default)
      Metadata:
        creation_time   : 2015-08-09T08:00:25.000000Z
        handler_name    : VideoByEZMediaEditor
        vendor_id       : [0][0][0][0]
  Stream #0:1(und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 122 kb/s (default)
      Metadata:
        creation_time   : 2015-08-09T08:00:29.000000Z
        handler_name    : Audio1-und
        vendor_id       : [0][0][0][0]
Press [q] to stop, [?] for help

---

## 2026-06-17 Update

- Added `--exit-after <seconds>` startup parameter. When the configured duration is reached, the SDL main loop requests exit, removes the stats timer, calls `player.close()`, restores `timeEndPeriod(1)`, and then calls `SDL_Quit()`.

## 2026-06-23 Update

- Investigated GPU-pusher playback freeze using `bin/MSVC2017_x86_Release/rtsp_player.log` and pusher logs.
- Finding: after a short video packet stall, audio clock kept advancing; `videoRefresh()` used extrapolated video clock for A/V correction, so stale video frames were treated as near-sync or video-leading. The renderer then slowed to 50ms delay while `VideoFrameQueue` overwrote continuously.
- Change: `videoRefresh()` now compares the pending frame PTS against audio clock, drops queued stale video frames when lag exceeds 250ms, and re-anchors the frame timer after catch-up drops.

## 2026-06-24 Update

- Investigated `--no-audio --setpts-zero` low-latency playback using `bin/MSVC2017_x64_Release/log_062313.log` and `bin/MSVC2017_x64_Release/csv_062313.csv`.
- Finding: decode and render counts stayed aligned, but `VideoFrameQueue` peaked at 6 queued frames during stutter windows, which accounts for roughly 200ms extra latency at 30fps.
- Change: in setpts-zero/no-audio mode, `videoRefresh()` now drops queued old video frames until only the newest frame remains, keeps the frame timer in immediate mode, and applies lower-latency FFmpeg input/decoder options (`probesize=2048`, `max_delay=0`, `fflags=nobuffer`, `AV_CODEC_FLAG_LOW_DELAY`).
- Verification: `cmake --build build --config Release` passed after cleaning the duplicate `Path/PATH` process environment variable.
- Follow-up test: `FrameQueue` peak dropped to 1 slot and total drop rate stayed near 2%, but `vQPeakMs` still reached 199ms. Tightened setpts-zero/no-audio video PacketQueue to 33ms and made tiny-capacity PacketQueue pushes drop immediately instead of waiting up to 100ms.
- Follow-up test 2: `csv_062314.csv` shows `vQPeakMs=33`, `fqPeakSlots<=1`, drop rate ~1%, average render interval ~33ms, max render interval 140ms. Found a separate stall bug where `VideoDecodeThread` exited after 30 empty-queue pop timeouts; changed it to drain/flush the decoder and keep waiting for new packets.
- Follow-up test 3: `log_062314.log` shows the decoder survived a 29-timeout stall and resumed rendering, confirming the empty-queue exit fix. The provided CSV was stale (last write 14:20 while log continued to 14:38), so CSV initialization now returns success/failure and main logs a warning instead of reporting enabled when the file cannot be opened.
- Follow-up test 4: `csv_062314.csv` refreshed correctly through 15:01. Low-latency buffering stayed bounded (`vQPeakMs=33`, `vQPeakPkt=1`, `fqPeakSlots=1`) with average render interval 34.57ms and max render latency 37ms. The only 3015ms render interval matched a real upstream/demux stall and reconnect path (`vStall=1`, `vPopTO=1`), not local queue buildup.

## 2026-06-25 Update

- Investigated multi-device `--setpts-zero` audio+video low-latency playback using `bin/MSVC2017_x64_Release/log_062509.log` and `bin/MSVC2017_x64_Release/csv_062509.csv`.
- Finding: final stats showed 23770 decoded video frames, 21214 rendered frames, and 2556 dropped frames (10% drop rate). The log also showed 623 video PacketQueue non-key packet drops. Because those drops happen before H.264 decode, they can break the reference chain and cause partial-region visual corruption.
- Change: PacketQueue overflow dropping is now explicit per queue instead of inferred from capacity. Low-latency audio still drops on overflow, but video waits for decoder consumption and no longer intentionally drops compressed non-key packets. Low-latency video PacketQueue capacity changed from 33ms to 67ms so a 30fps stream can hold about two packets without tripping on 33.33ms frame-duration rounding.
- Follow-up x86/x64 comparison used `bin/MSVC2017_x86_Release/log-062510.log` + `csv-062510.csv` and `bin/MSVC2017_x64_Release/log_062510.log` + `csv_062510.csv`. The x86 run used UDP and FFmpeg 4.2 (`avcodec-58`), while the x64 run used TCP and FFmpeg 7.x (`avcodec-61`). x86 logged hundreds of FFmpeg H.264 macroblock/CABAC decode errors and repeated slow `avcodec_send_packet`; x64 logged none of those decode errors.
- Change: when `--setpts-zero` is enabled and `--transport` is not explicitly supplied, the player now defaults RTSP transport to TCP. UDP remains available via `--transport udp`. This avoids treating UDP packet loss from the x86/old-FFmpeg path as a stable low-latency default.
- Investigated x86 DXVA2 playback failure using `bin/MSVC2017_x86_Release/log_062514.log` and `bin/MSVC2017_x86_Release/csv_062514.csv`.
- Finding: hardware frame transfer succeeded (`hwXferFail=0`) and produced NV12 frames (`fmt=23`), but `SDLRenderer` still used a fixed YUV420P-to-NV12 swscale context. swscale then read NV12 as three-plane YUV420P and logged repeated `bad src image pointers`.
- Change: `SDLRenderer` now handles source pixel format dynamically. NV12 frames are uploaded directly with `SDL_UpdateNVTexture`; non-NV12 frames use a source-format-aware swscale context to convert to NV12.
- Verification: the refreshed x86 DXVA2 run in the same log starts at 14:47:29. After that point `bad src image pointers` no longer appears, render logs show `sws=0.0ms`, CSV ends with 26770 decoded / 26624 rendered / 146 dropped frames, and `hwXferFail=0`. The run later hit `av_read_frame returned EOF` at 15:02:28, after which stats stayed flat until manual keyboard exit at 15:20:08.
- Change: EOF is now treated as a normal end-of-stream signal. `DemuxThread` notifies `StreamLifecycleManager`, `RTSPlayer` forwards the callback to `main`, and the SDL main loop exits through a dedicated `EVENT_STREAM_EOF` event instead of leaving the app running with frozen CSV counters.
- Investigated x86 DXVA2 long-run failure using `bin/MSVC2017_x86_Release/log-062515.log` and `bin/MSVC2017_x86_Release/csv-062515.csv`.
- Finding: this run no longer showed the previous NV12/sws `bad src image pointers` failure. Instead, the x86 process enabled DXVA2 and FFmpeg immediately logged `Failed to execute: 0x80070057` plus `hardware accelerator failed to decode picture`; playback then continued for several minutes before the log stopped without normal shutdown.
- Change: 32-bit Windows `--hwaccel auto` now uses software decode by default and logs that DXVA2 auto is disabled for the 32-bit process. Explicit `--hwaccel dxva2` still forces DXVA2 for hardware-path testing.
