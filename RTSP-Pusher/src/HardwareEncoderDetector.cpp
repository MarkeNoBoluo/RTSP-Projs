#include "HardwareEncoderDetector.h"
#include "logger/Logger.h"
#include <cstdio>
#include <cstring>

#ifdef __linux__
#include <unistd.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
}

// ── Known H.264 encoder names ────────────────────────────────────────
struct EncoderEntry {
    const char* name;
    const char* description;
    AVPixelFormat pixFmt;
};

static const EncoderEntry kEncoders[] = {
    {"libx264",   "libx264 software encoder",              AV_PIX_FMT_YUV420P},
    {"h264_qsv",  "Intel Quick Sync Video H.264 encoder",  AV_PIX_FMT_NV12},
    {"h264_amf",  "AMD AMF H.264 encoder",                 AV_PIX_FMT_YUV420P},
    {"h264_nvenc","NVIDIA NVENC H.264 encoder",            AV_PIX_FMT_YUV420P},
    {"h264_vaapi","VAAPI hardware H.264 encoder",          AV_PIX_FMT_VAAPI},
};

// ── Minimal open/close probe for one encoder ─────────────────────────
// Returns true if the encoder compiles AND can open successfully.
static bool probeEncoderOpenable(const EncoderEntry& entry) {
    const AVCodec* codec = avcodec_find_encoder_by_name(entry.name);
    if (!codec) return false;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    ctx->width     = 1920;
    ctx->height    = 1080;
    ctx->pix_fmt   = entry.pixFmt;
    ctx->time_base = {1, 30};
    ctx->framerate = {30, 1};
    ctx->gop_size  = 30;
    ctx->max_b_frames = 0;
    ctx->flags    |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ctx->bit_rate  = 4000000;

    // QSV-specific
    if (std::strstr(entry.name, "qsv")) {
        av_opt_set(ctx->priv_data, "async_depth", "1", 0);
        av_opt_set(ctx->priv_data, "low_power",  "1", 0);
    }

    // VAAPI-specific: create temporary hw device so avcodec_open2 can succeed
    AVBufferRef* probeHwDev = nullptr;
    if (std::strstr(entry.name, "vaapi")) {
        const char* drmDev = "/dev/dri/renderD128";
#ifdef __linux__
        if (::access(drmDev, R_OK | W_OK) != 0) {
            avcodec_free_context(&ctx);
            return false;
        }
#endif
        // Suppress FFmpeg internal error log during probe
        int prevLevel = av_log_get_level();
        av_log_set_level(AV_LOG_QUIET);
        int hwret = av_hwdevice_ctx_create(&probeHwDev, AV_HWDEVICE_TYPE_VAAPI,
                                           drmDev, nullptr, 0);
        av_log_set_level(prevLevel);
        if (hwret != 0 || !probeHwDev) {
            avcodec_free_context(&ctx);
            return false;
        }
        ctx->hw_device_ctx = av_buffer_ref(probeHwDev);
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    }

    int ret = avcodec_open2(ctx, codec, nullptr);
    if (probeHwDev) av_buffer_unref(&probeHwDev);
    avcodec_free_context(&ctx);
    return ret == 0;
}

// ── Public API ───────────────────────────────────────────────────────

void printAvailableEncoders() {
    printf("H.264 encoder availability:\n");
    printf("  %-16s %-10s %-10s %s\n", "Name", "Compiled", "Openable", "Description");
    printf("  %-16s %-10s %-10s %s\n", "----", "--------", "--------", "-----------");

    for (const auto& e : kEncoders) {
        bool compiled = (avcodec_find_encoder_by_name(e.name) != nullptr);
        bool openable = compiled && probeEncoderOpenable(e);

        printf("  %-16s %-10s %-10s %s\n",
               e.name,
               compiled ? "yes" : "no",
               openable ? "yes" : "no",
               e.description);

        // If compiled but not openable, note the reason
        if (compiled && !openable) {
            printf("    ^-- compiled in FFmpeg but failed to open (driver/runtime issue)\n");
        }
    }

    printf("\nUsage:\n");
    printf("  --hw-encoder off          Use libx264 (default)\n");
    printf("  --hw-encoder auto         Try QSV -> VAAPI -> NVENC -> libx264\n");
    printf("  --hw-encoder qsv          Intel Quick Sync (explicit, fail if unavailable)\n");
    printf("  --hw-encoder vaapi        VAAPI (explicit, fail if unavailable)\n");
    printf("  --hw-encoder nvenc        NVIDIA NVENC (explicit, fail if unavailable)\n");
    printf("  --hw-encoder <codec-name> Use a specific encoder by name\n");
}

const char* resolveEncoderName(const char* hwEncoderSetting) {
    if (!hwEncoderSetting || std::strcmp(hwEncoderSetting, "off") == 0) {
        return "libx264";
    }

    if (std::strcmp(hwEncoderSetting, "auto") == 0) {
        return "h264_qsv";  // caller handles probe + fallback
    }

    if (std::strcmp(hwEncoderSetting, "qsv") == 0) {
        if (avcodec_find_encoder_by_name("h264_qsv")) {
            return "h264_qsv";
        }
        return nullptr;
    }

    if (std::strcmp(hwEncoderSetting, "nvenc") == 0) {
        if (avcodec_find_encoder_by_name("h264_nvenc")) {
            return "h264_nvenc";
        }
        return nullptr;
    }

    if (std::strcmp(hwEncoderSetting, "vaapi") == 0) {
        if (avcodec_find_encoder_by_name("h264_vaapi")) {
            return "h264_vaapi";
        }
        return nullptr;
    }

    // Direct codec name
    if (avcodec_find_encoder_by_name(hwEncoderSetting)) {
        return hwEncoderSetting;
    }

    return nullptr;
}
