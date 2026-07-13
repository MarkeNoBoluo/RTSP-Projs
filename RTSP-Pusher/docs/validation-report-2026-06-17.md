# RTSP-Pusher 环境验证报告

**日期**: 2026-06-17
**阶段**: Phase 0 环境确认

## 校验结果

| 校验点 | 方法 | 结果 | 详情 |
|--------|------|------|------|
| FFmpeg DLL 可用 | `ffmpeg.exe -version` | ✅ 通过 | FFmpeg 4.2.1, gcc 9.1.1, 含 libx264/libx265/AAC |
| gdigrab 可用 | `ffmpeg.exe -devices` | ✅ 通过 | `D  gdigrab  GDI API Windows frame grabber` |
| libx264 可用 | `ffmpeg.exe -encoders` | ✅ 通过 | `V libx264 H.264 / AVC / MPEG-4 AVC` |
| AAC 编码器可用 | `ffmpeg.exe -encoders` | ✅ 通过 | `A aac  AAC (Advanced Audio Coding)` |
| RTSP 输出可用 | `ffmpeg.exe -formats` | ✅ 通过 | `DE rtsp  RTSP output` |
| SDL2 DLL 可用 | 检查 `3rd/SDL2/lib/x86/SDL2.dll` | ✅ 通过 | 文件存在, x86 版本 |
| 音频设备可枚举 | dshow 设备列表 | ✅ 通过 | 麦克风阵列 (Realtek(R) Audio) 可用 |

## 依赖清单

### FFmpeg (4.2.1)
- **include**: libavcodec, libavdevice, libavfilter, libavformat, libavutil, libpostproc, libswresample, libswscale
- **lib**: avcodec.lib, avdevice.lib, avfilter.lib, avformat.lib, avutil.lib, postproc.lib, swresample.lib, swscale.lib
- **bin**: 所有对应 DLL + ffmpeg.exe, ffplay.exe, ffprobe.exe

### SDL2
- **include**: 完整 SDL2 头文件 (含 SDL_audio.h, SDL_events.h 等)
- **lib/x86**: SDL2.dll, SDL2.lib, SDL2main.lib

## 结论

所有依赖和能力已就绪，可进入 Phase 1 工程骨架开发。
