# RTSP-Projs

Windows 平台 RTSP 流媒体项目集，包含播放器（RTSP-Player）和推流器（RTSP-Pusher）两个应用程序，通过外部 RTSP 服务器（如 MediaMTX）间接通信。

## 子项目

| 项目 | 说明 |
|------|------|
| [RTSP-Player](RTSP-Player/) | SDL2 + FFmpeg RTSP 流媒体播放器，拉流解码，渲染视频/音频 |
| [RTSP-Pusher](RTSP-Pusher/) | 桌面屏幕 + 音频采集编码，推送至 RTSP 服务器 |
| [RTSP-Server](RTSP-Server/) | 占位目录，实际 RTSP 服务由外部 MediaMTX 提供 |

## 构建

- **编译器**: MSVC 2017，C++17，`/utf-8`
- **构建系统**: CMake 3.16+
- **依赖**: FFmpeg、SDL2（预编译库置于各子项目 `3rd/` 下）
- **架构**: x86（FFmpeg 4.2.x）和 x64（FFmpeg 7.x）分别构建

```bash
# RTSP-Player
cd RTSP-Player
cmake -S . -B build
cmake --build build --config Release

# RTSP-Pusher
cd RTSP-Pusher
cmake -B build-x86 -G "Visual Studio 15 2017" -A Win32
cmake --build build-x86 --config Release
```

## 目录结构

```
├── RTSP-Player/          # RTSP 播放器
│   ├── src/              # 源代码
│   ├── 3rd/              # FFmpeg + SDL2 预编译库
│   └── docs/             # 设计文档与指标说明
├── RTSP-Pusher/          # RTSP 推流器
│   ├── src/              # 源代码
│   ├── 3rd/              # FFmpeg + SDL2 预编译库
│   └── docs/             # 设计文档与优化方案
├── RTSP-Server/          # 服务端占位
├── docs/                 # 跨项目文档
└── CLAUDE.md             # Claude Code 项目指引
```
