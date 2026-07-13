# RTSP Player v2

纯 C++17 实现的 SDL2 + FFmpeg RTSP 流媒体播放器，无 Qt 依赖。

## 构建

```bash
cmake -S . -B build
cmake --build build --config Release    # 或 Debug
```

- **编译器**: MSVC 2017，C++17，`/utf-8`
- **架构**: x86 / x64 自动检测（CMake 根据指针大小判定）
- **输出**: `bin/MSVC2017_<架构>_<配置>/RTSP-Player.exe`

运行时需在输出目录下执行（DLL 通过 POST_BUILD 自动拷贝到位）：

```bash
cd bin/MSVC2017_x86_Release
./RTSP-Player.exe --url rtsp://192.168.1.100:554/stream
```

## 命令行参数

| 参数 | 说明 |
|------|------|
| `--url <url>` | RTSP 流地址（必填，未提供时自动生成默认地址） |
| `--log <路径>` | 日志文件路径（默认 `rtsp_player.log`） |
| `--csv [路径]` | CSV 统计文件路径（默认 `rtsp_player_stats.csv`，路径可选） |
| `--no-csv` | 禁用 CSV 统计输出 |
| `--fullscreen` | 启动时全屏 |
| `--windowed` | 启动时窗口模式（默认） |
| `--transport <tcp\|udp>` | RTSP 传输协议（默认 `tcp`） |
| `--title <标题>` | 窗口标题（默认 `"RTSP Player"`） |
| `--exit-after <秒>` | 指定秒数后自动退出 |
| `--no-audio` | 禁用音频，纯视频模式（延迟最低） |
| `--setpts-zero` | 低延迟模式：无音频时逐帧直出，有音频时缩减队列深度（延迟约 300ms） |
| `--hwaccel <auto\|dxva2\|none>` | 硬件解码模式（默认 `auto`，x86 下 auto 自动降级为软件解码） |
| `--help` | 显示帮助信息 |

旧式位置参数形式（`argv[1]`=url, `argv[2]`=log）仍可用但已废弃。

## 键盘操作

| 按键 | 功能 |
|------|------|
| `Esc` / `Q` | 退出 |
| `F` | 切换全屏 / 窗口 |

## 整体架构

单进程多线程，主线程以约 1ms 间隔循环：`SDL_PollEvent` → `videoRefresh()` → `av_usleep` 补偿。

```
main()
 ├── SDL_Init (VIDEO | AUDIO | TIMER)
 ├── SDLRenderer — 窗口 + 渲染器 + YUV 纹理（swscale 转 NV12）
 ├── RTSPlayer — 外观类，封装全部播放逻辑
 │    ├── PlayerStateMachine — 7 状态原子状态机
 │    ├── StreamLifecycleManager — 管道生命周期管理，持有所有线程和 FFmpeg 上下文
 │    │    ├── DemuxThread        → 读取 AVPacket 并推入队列
 │    │    ├── VideoDecodeThread  → 解码视频帧，写入帧环形缓冲
 │    │    ├── AudioWorker        → 解码音频，重采样，写入音频环形缓冲
 │    │    └── SDLAudio           → SDL 音频回调读取环形缓冲，更新音频时钟
 │    ├── PacketQueue ×2（视频、音频）— 条件变量 + 序列号感知
 │    ├── VideoFrameQueue（24 槽）   — 解码与渲染间的帧环形缓冲
 │    ├── AVClock                    — 独立音视频 PTS 时钟，drift() 计算同步偏差
 │    └── PlayerStats                — std::atomic 指标 + CSV 导出
 └── SDL_AddTimer 驱动的 SDL_USEREVENT：
      EVENT_STATS (5s)    → 写 CSV 行
      EVENT_RECONNECT     → 触发重连
```

## 低延迟模式

| 模式 | 触发方式 | 视频队列 | 音频队列 | 环形缓冲 | 目标延迟 |
|------|----------|----------|----------|----------|----------|
| 正常 | 默认 | 200ms | 默认 | 默认 | ~500-800ms |
| 低延迟 A+V | `--setpts-zero`（有音频） | 67ms | 66ms | 60ms | ~300ms |
| 低延迟纯视频 | `--setpts-zero`（无音频）或 `--no-audio` | 67ms | — | — | ~1 帧 |

## 音视频同步

以音频时钟为基准，视频帧通过 `AVClock` 计算 `avDiff = framePts - audioClock`：

- 落后过多：丢弃帧追赶
- 仅剩 1 帧仍落后：进入快速追赶模式（无间歇连续渲染）
- 超前：适当拉伸帧间隔等待

## 依赖项

- **SDL2** — `3rd/SDL2/`
- **FFmpeg** — `3rd/FFmpeg/`
  - x86: FFmpeg 4.2.x（avcodec-58, avformat-58, avutil-56, swresample-3, swscale-5）
  - x64: FFmpeg 5+（avcodec-61, avformat-61, avutil-59, swresample-5, swscale-8）

## 日志

单例 `logger::Logger`（`src/logger/Logger.h`），线程安全，格式 `[LEVEL] file.cpp:行号 消息`。

## 代码约定

- C++17，`#pragma once`，成员变量 `m_` 前缀，4 空格缩进
- UTF-8 with BOM
- FFmpeg 头文件必须用 `extern "C" {}` 包裹
- `src/test_qt.cpp` 为 v1 时代遗留的 Qt 冒烟测试，不参与编译，勿删
