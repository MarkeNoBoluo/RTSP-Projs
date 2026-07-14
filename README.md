# RTSP-Projs

Windows / Linux (KylinV10-SP1) RTSP 流媒体项目集，包含播放器（RTSP-Player）和推流器（RTSP-Pusher）两个应用程序，通过外部 RTSP 服务器（如 MediaMTX）间接通信。

## 子项目

| 项目 | 说明 |
|------|------|
| [RTSP-Player](RTSP-Player/) | SDL2 + FFmpeg RTSP 流媒体播放器，拉流解码，渲染视频/音频 |
| [RTSP-Pusher](RTSP-Pusher/) | 桌面屏幕 + 音频采集编码，推送至 RTSP 服务器 |
| [RTSP-Server](RTSP-Server/) | 占位目录，实际 RTSP 服务由外部 MediaMTX 提供 |

## 平台支持

| 平台 | 架构 | FFmpeg | 编译器 | 硬件编码 |
|------|------|--------|--------|----------|
| Windows | x86 / x64 | 4.2.x / 7.x | MSVC 2017 | QSV (x64) |
| Linux (KylinV10-SP1) | x86_64 | 4.4.6 | GCC 12.1.0 | VAAPI |

## 构建

**编译器**: C++17，Windows 使用 `/utf-8`

### Linux

```bash
# RTSP-Player
./build-rtsp-player.sh build

# RTSP-Pusher
./build-rtsp-pusher.sh build

# 打包部署（含 .so 依赖）
./build-rtsp-pusher.sh deploy
```

### Windows

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

## 硬件编码

### VAAPI (Linux)

```bash
./RTSP-Pusher --hw-encoder vaapi --drm-device /dev/dri/renderD128 \
    --capture-size 1920x1080 --output-size 1920x1080 --fps 30 \
    --url rtsp://192.168.42.116:25544/live
```

推流器通过启动编码（priming encode）提取 SPS/PPS 构建 extradata，解决部分 VAAPI 驱动不支持 packed sequence header 导致 RTSP SDP 缺失 `sprop-parameter-sets` 的问题。

### QSV (Windows x64)

```bash
RTSP-Pusher.exe --hw-encoder qsv --capture-size 2560x1440 --output-size 1920x1080
```

## 目录结构

```
├── RTSP-Player/              # RTSP 播放器
│   ├── src/                  # 源代码
│   ├── 3rd/                  # FFmpeg + SDL2 预编译库 (Windows)
│   └── docs/                 # 设计文档与指标说明
├── RTSP-Pusher/              # RTSP 推流器
│   ├── src/                  # 源代码
│   ├── 3rd/                  # FFmpeg + SDL2 预编译库 (Windows)
│   └── docs/                 # 设计文档与优化方案
├── RTSP-Server/              # 服务端占位
├── docs/                     # 跨项目文档
├── build-rtsp-player.sh      # Player Linux 构建脚本
├── build-rtsp-pusher.sh      # Pusher Linux 构建/部署脚本
└── CLAUDE.md                 # Claude Code 项目指引
```
