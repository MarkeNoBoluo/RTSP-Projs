根据你提供的所有信息，这台银河麒麟主机的软硬件情况已经比较完整，下面整理成一份主机配置文档。

---

# 银河麒麟开发主机信息

## 一、基本信息

| 项目                | 信息                                      |
| ----------------- | --------------------------------------- |
| 操作系统              | 银河麒麟 Linux（Ubuntu 20.04 系，GCC 9）        |
| CPU 架构            | x86_64                                  |
| CPU               | ZHAOXIN KaiXian KX-U6580 @ 2.5GHz       |
| CPU 厂商            | Shanghai Zhaoxin（兆芯）                    |
| CPU 核心            | 8 Core / 8 Thread                       |
| CPU 指令集           | SSE4.2、AVX、AES、SM3、SM4、BMI1、BMI2、F16C 等 |
| GPU               | Zhaoxin ZX-E C-960 GPU                  |
| DRM Render Device | `/dev/dri/renderD128`                   |
| OpenGL Renderer   | ZX C960 GPU                             |

其中，ZX-E C-960 是兆芯 KX-6000 系列处理器集成 GPU（LuJiaZui 平台），支持 OpenGL、VA-API 等图形与视频加速能力。([forum-3dcenter.org][1])

---

# 二、GPU 驱动状态

## DRM 驱动

已正常加载：

```text
zx
zx_core
drm
drm_kms_helper
```

GPU 已成功识别：

```text
00:01.0 VGA compatible controller:
Zhaoxin ZX-E C-960 GPU
```

Render Node：

```text
/dev/dri/card0
/dev/dri/renderD128
```

说明：

* ✓ DRM 正常
* ✓ KMS 正常
* ✓ Render Node 正常

---

## OpenGL

Renderer：

```text
OpenGL renderer string:
ZX C960 GPU
```

说明：

* GPU 已参与 OpenGL 渲染
* 不是 Mesa llvmpipe 软件渲染
* OpenGL 驱动正常

---

## Kernel Driver

驱动版本：

```text
Version:
21.00.38
Build:
2021-06-10
```

DRM 初始化：

```text
Initialized zx 33.0.56
```

显存：

```text
Video Memory
512 MB
```

---

# 三、VAAPI 视频加速

## libva

```text
VA-API Version
1.7

libva
2.7.0
```

---

## VAAPI Driver

Driver：

```text
ZX_VA
```

Driver 文件：

```text
/usr/lib/x86_64-linux-gnu/dri/zx_drv_video.so
```

初始化成功：

```text
Found init function __vaDriverInit_1_0

va_openDriver() returns 0
```

说明：

**ZX 官方 VAAPI Driver 工作正常。**

---

## 注意事项

默认环境下：

```bash
vainfo
```

失败。

原因是系统没有自动识别 Driver。

必须指定：

```bash
export LIBVA_DRIVER_NAME=zx
```

之后：

```bash
vainfo
```

即可正常工作。

建议写入：

```bash
~/.bashrc
```

例如：

```bash
export LIBVA_DRIVER_NAME=zx
```

这样以后 FFmpeg、mpv、GStreamer 等都会自动使用 ZX VA Driver。

---

# 四、FFmpeg 编译信息

当前 FFmpeg：

```
Version
4.4.6
```

已开启：

```
--enable-vaapi
--enable-libx264
--enable-libfdk-aac
--enable-openssl
--enable-sdl2
```

支持 Hardware Acceleration：

```
VAAPI
VDPAU
```

---

## 编码器

支持：

```
libx264
h264_vaapi
h264_v4l2m2m
```

说明：

支持：

* CPU H264 编码
* VAAPI H264 硬编码

---

## 解码器

支持：

```
H264
h264_v4l2m2m
```

结合：

```
-hwaccel vaapi
```

即可实现 H264 硬件解码。

---

# 五、ZX C960 支持的视频能力

根据 `vainfo`：

| Codec            | 解码 | 编码 |
| ---------------- | -- | -: |
| MPEG2            | ✓  |  — |
| MPEG4 ASP        | ✓  |  — |
| H263             | ✓  |  — |
| H264 Baseline    | ✓  |  ✓ |
| H264 Main        | ✓  |  ✓ |
| H264 High        | ✓  |  ✓ |
| H264 MVC         | ✓  |  ✓ |
| H264 Stereo      | ✓  |  ✓ |
| HEVC Main        | ✓  |  ✓ |
| HEVC Main10      | ✓  |  ✓ |
| JPEG             | ✓  |  ✓ |
| VP8              | ✓  |  — |
| Video Processing | ✓  |  — |

因此：

**H.264、H.265（HEVC，包括 Main10）均支持硬件编码和硬件解码。**

---

# 六、FFmpeg 使用方式

## H264 硬解

```bash
ffmpeg \
-hwaccel vaapi \
-hwaccel_device /dev/dri/renderD128 \
-hwaccel_output_format vaapi \
-i input.mp4
```

---

## H264 硬编码

```bash
ffmpeg \
-vaapi_device /dev/dri/renderD128 \
-i input.mp4 \
-vf "format=nv12,hwupload" \
-c:v h264_vaapi \
output.mp4
```

---

## HEVC 硬编码

```bash
ffmpeg \
-vaapi_device /dev/dri/renderD128 \
-i input.mp4 \
-vf "format=nv12,hwupload" \
-c:v hevc_vaapi \
output.mp4
```

---

# 七、对于 FFmpeg + Qt 项目的建议

针对你目前开发的 **FFmpeg + SDL + Qt RTSP Player / RTSP Pusher**，推荐统一设计如下：

```
HardwareDevice
│
├── Software
├── VAAPI
├── DXVA2
├── D3D11VA
└── QSV（可选）
```

Linux（银河麒麟）平台：

```
AVHWDeviceType
        │
        ▼
AV_HWDEVICE_TYPE_VAAPI
        │
        ▼
ZX_VA
        │
        ▼
ZX-E C960
```

Windows 平台：

```
AV_HWDEVICE_TYPE_D3D11VA
```

这样可以实现同一套播放器代码跨 Windows、银河麒麟等平台运行，只需在初始化阶段根据平台选择对应的硬件设备类型即可。

---

## 综合评价

这台主机已经具备完整的国产桌面开发能力，尤其适合作为 FFmpeg/Qt 多媒体开发平台：

* **CPU**：兆芯 KX-U6580（8 核 x86_64）
* **GPU**：ZX-E C-960 集成显卡
* **OpenGL**：正常启用 GPU 硬件渲染
* **VA-API**：正常工作（需设置 `LIBVA_DRIVER_NAME=zx`）
* **FFmpeg**：支持 VAAPI H.264/H.265 硬件编码与硬件解码
* **适用场景**：RTSP 拉流、RTSP 推流、视频播放器、屏幕录制、实时转码、Qt + FFmpeg 多媒体应用开发。

[1]: https://www.forum-3dcenter.org/vbulletin/archive/index.php/t-572719-p-3.html?utm_source=chatgpt.com "VIA - Zhaoxin x86-Prozessoren [Archiv] - Seite 3 - 3DCenter Forum"
