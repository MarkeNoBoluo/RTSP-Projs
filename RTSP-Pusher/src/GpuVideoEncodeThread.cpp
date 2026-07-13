#include "GpuVideoEncodeThread.h"
#include "EncodedPacketQueue.h"
#include "PtsUtils.h"
#include "logger/Logger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

static void logAvError(const char* prefix, int ret) {
    char errBuf[256];
    av_strerror(ret, errBuf, sizeof(errBuf));
    LOG_ERROR("%s: %s", prefix, errBuf);
}

GpuVideoEncodeThread::GpuVideoEncodeThread(const PusherConfig* config,
                                           EncodedPacketQueue* outputQueue,
                                           PusherStats* stats,
                                           GpuBackend backend)
    : m_config(config), m_outputQueue(outputQueue), m_stats(stats), m_backend(backend) {}

GpuVideoEncodeThread::~GpuVideoEncodeThread() {
    stop();
}

bool GpuVideoEncodeThread::start(int serial) {
    m_serial = serial;
    m_abort = false;

    const char* backendName = (m_backend == GpuBackend::NVENC) ? "CUDA/NVENC" : "QSV";
    const char* encoderName = (m_backend == GpuBackend::NVENC) ? "h264_nvenc" : "h264_qsv";

    if (!openFilterGraph()) {
        LOG_ERROR("[gpu-video] Failed to open ddagrab/%s filter graph", backendName);
        return false;
    }
    if (!pullInitialFrame()) {
        LOG_ERROR("[gpu-video] Failed to pull initial ddagrab/%s frame", backendName);
        return false;
    }
    if (!openEncoder()) {
        LOG_ERROR("[gpu-video] Failed to open %s encoder", encoderName);
        return false;
    }

    m_thread = std::thread(&GpuVideoEncodeThread::run, this);
    return true;
}

void GpuVideoEncodeThread::stop() {
    m_abort = true;
}

void GpuVideoEncodeThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    close();
}

void GpuVideoEncodeThread::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}

void GpuVideoEncodeThread::setSerial(int serial) {
    m_serial = serial;
}

bool GpuVideoEncodeThread::openFilterGraph() {
    m_filterGraph = avfilter_graph_alloc();
    if (!m_filterGraph) {
        LOG_ERROR("[gpu-video] avfilter_graph_alloc failed");
        return false;
    }

    const bool isNvenc = (m_backend == GpuBackend::NVENC);
    const char* scaleFilterName = isNvenc ? "not-used" : "scale_qsv";
    const char* hwmapArgs = "derive_device=qsv";
    const char* backendLabel = isNvenc ? "d3d11" : "qsv";

    const AVFilter* ddagrab = avfilter_get_by_name("ddagrab");
    const AVFilter* hwmap = isNvenc ? nullptr : avfilter_get_by_name("hwmap");
    const AVFilter* scaleFilter = isNvenc ? nullptr : avfilter_get_by_name(scaleFilterName);
    const AVFilter* sink = avfilter_get_by_name("buffersink");
    if (!ddagrab || (!isNvenc && (!hwmap || !scaleFilter)) || !sink) {
        LOG_ERROR("[gpu-video] Required filters missing: ddagrab=%s hwmap=%s %s=%s buffersink=%s",
                  ddagrab ? "yes" : "no",
                  isNvenc ? "not-needed" : (hwmap ? "yes" : "no"),
                  scaleFilterName,
                  isNvenc ? "not-needed" : (scaleFilter ? "yes" : "no"),
                  sink ? "yes" : "no");
        return false;
    }

    char ddagrabArgs[256];
    snprintf(ddagrabArgs, sizeof(ddagrabArgs),
             "output_idx=%d:framerate=%d:video_size=%dx%d:offset_x=%d:offset_y=%d:"
             "draw_mouse=1:output_fmt=bgra:dup_frames=1",
             m_config->screenIndex > 0 ? (m_config->screenIndex - 1) : 0,
             m_config->captureFps,
             m_config->captureWidth,
             m_config->captureHeight,
             m_config->captureOffsetX,
             m_config->captureOffsetY);

    int ret = avfilter_graph_create_filter(&m_sourceCtx, ddagrab, "ddagrab",
                                           ddagrabArgs, nullptr, m_filterGraph);
    if (ret < 0) {
        logAvError("[gpu-video] create ddagrab failed", ret);
        return false;
    }

    ret = avfilter_graph_create_filter(&m_sinkCtx, sink, "buffersink",
                                       nullptr, nullptr, m_filterGraph);
    if (ret < 0) {
        logAvError("[gpu-video] create buffersink failed", ret);
        return false;
    }

    if (isNvenc) {
        ret = avfilter_link(m_sourceCtx, 0, m_sinkCtx, 0);
    } else {
        ret = avfilter_graph_create_filter(&m_hwmapCtx, hwmap, "hwmap",
                                           hwmapArgs, nullptr, m_filterGraph);
        if (ret < 0) {
            logAvError("[gpu-video] create hwmap failed", ret);
            return false;
        }

        char scaleArgs[128];
        snprintf(scaleArgs, sizeof(scaleArgs), "w=%d:h=%d:format=nv12",
                 m_config->outputWidth, m_config->outputHeight);
        ret = avfilter_graph_create_filter(&m_scaleCtx, scaleFilter, scaleFilterName,
                                           scaleArgs, nullptr, m_filterGraph);
        if (ret < 0) {
            LOG_ERROR("[gpu-video] create %s failed", scaleFilterName);
            logAvError("[gpu-video] create scale filter failed", ret);
            return false;
        }

        ret = avfilter_link(m_sourceCtx, 0, m_hwmapCtx, 0);
        if (ret >= 0) ret = avfilter_link(m_hwmapCtx, 0, m_scaleCtx, 0);
        if (ret >= 0) ret = avfilter_link(m_scaleCtx, 0, m_sinkCtx, 0);
    }
    if (ret < 0) {
        logAvError("[gpu-video] filter link failed", ret);
        return false;
    }

    ret = avfilter_graph_config(m_filterGraph, nullptr);
    if (ret < 0) {
        logAvError("[gpu-video] avfilter_graph_config failed", ret);
        return false;
    }

    if (isNvenc) {
        LOG_INFO("[gpu-video] filter graph opened: ddagrab -> %s frames -> buffersink",
                 backendLabel);
    } else {
        LOG_INFO("[gpu-video] filter graph opened: ddagrab -> hwmap(%s) -> %s(%dx%d,nv12)",
                 backendLabel, scaleFilterName,
                 m_config->outputWidth, m_config->outputHeight);
    }
    return true;
}

bool GpuVideoEncodeThread::pullInitialFrame() {
    m_pendingFrame = av_frame_alloc();
    if (!m_pendingFrame) {
        LOG_ERROR("[gpu-video] av_frame_alloc(initial) failed");
        return false;
    }

    int ret = av_buffersink_get_frame(m_sinkCtx, m_pendingFrame);
    if (ret < 0) {
        logAvError("[gpu-video] av_buffersink_get_frame(initial) failed", ret);
        av_frame_free(&m_pendingFrame);
        return false;
    }

    if (!m_pendingFrame->hw_frames_ctx) {
        LOG_ERROR("[gpu-video] initial hardware frame has no hw_frames_ctx");
        av_frame_free(&m_pendingFrame);
        return false;
    }

    m_hwFramesRef = av_buffer_ref(m_pendingFrame->hw_frames_ctx);
    if (!m_hwFramesRef) {
        LOG_ERROR("[gpu-video] av_buffer_ref(initial hw_frames_ctx) failed");
        av_frame_free(&m_pendingFrame);
        return false;
    }

    LOG_INFO("[gpu-video] initial hardware frame received: %dx%d fmt=%d",
             m_pendingFrame->width, m_pendingFrame->height, m_pendingFrame->format);
    return true;
}

bool GpuVideoEncodeThread::openEncoder() {
    const bool isNvenc = (m_backend == GpuBackend::NVENC);

    if (isNvenc) {
        // ── NVENC path: single attempt ──
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) {
            LOG_ERROR("[gpu-video] h264_nvenc encoder not found");
            return false;
        }

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            LOG_ERROR("[gpu-video] avcodec_alloc_context3 failed");
            return false;
        }

        int encodeWidth = m_pendingFrame ? m_pendingFrame->width : m_config->captureWidth;
        int encodeHeight = m_pendingFrame ? m_pendingFrame->height : m_config->captureHeight;
        if (encodeWidth != m_config->outputWidth || encodeHeight != m_config->outputHeight) {
            LOG_WARN("[gpu-video] NVENC ddagrab path uses D3D11 frames directly; "
                     "pure-GPU scaling is unavailable in this FFmpeg build. "
                     "Encoding captured size %dx%d instead of requested output %dx%d.",
                     encodeWidth, encodeHeight,
                     m_config->outputWidth, m_config->outputHeight);
        }

        ctx->width = encodeWidth;
        ctx->height = encodeHeight;
        ctx->time_base = {1, 90000};
        ctx->framerate = {m_config->captureFps, 1};
        ctx->pix_fmt = AV_PIX_FMT_D3D11;
        ctx->gop_size = m_config->gopSize;
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ctx->bit_rate = m_config->videoBitrate * 1000;
        ctx->rc_max_rate = m_config->videoMaxrate * 1000;
        ctx->rc_buffer_size = m_config->videoBufsize * 1000;
        ctx->hw_frames_ctx = av_buffer_ref(m_hwFramesRef);
        if (!ctx->hw_frames_ctx) {
            LOG_ERROR("[gpu-video] av_buffer_ref(encoder hw_frames_ctx) failed");
            avcodec_free_context(&ctx);
            return false;
        }

        av_opt_set(ctx->priv_data, "preset", "p4", 0);
        av_opt_set(ctx->priv_data, "tune", "ll", 0);
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(ctx->priv_data, "delay", "0", 0);
        av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
        av_opt_set(ctx->priv_data, "b_adapt", "0", 0);

        int ret = avcodec_open2(ctx, codec, nullptr);
        if (ret == 0) {
            m_codecCtx = ctx;
            LOG_INFO("[gpu-video] encoder opened: h264_nvenc  pix_fmt=D3D11  %dx%d  bitrate=%dkbps  "
                     "maxrate=%dkbps  bufsize=%dkbits  gop=%d",
                     encodeWidth, encodeHeight,
                     m_config->videoBitrate, m_config->videoMaxrate,
                     m_config->videoBufsize, m_config->gopSize);
            return true;
        }

        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[gpu-video] h264_nvenc avcodec_open2 failed: %s", errBuf);
        avcodec_free_context(&ctx);
        return false;
    }

    // ── QSV path: low_power retry loop ──
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_qsv");
    if (!codec) {
        LOG_ERROR("[gpu-video] h264_qsv encoder not found");
        return false;
    }

    for (int lowPower = 1; lowPower >= 0; --lowPower) {
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            LOG_ERROR("[gpu-video] avcodec_alloc_context3 failed");
            return false;
        }

        ctx->width = m_config->outputWidth;
        ctx->height = m_config->outputHeight;
        ctx->time_base = {1, 90000};
        ctx->framerate = {m_config->captureFps, 1};
        ctx->pix_fmt = AV_PIX_FMT_QSV;
        ctx->gop_size = m_config->gopSize;
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ctx->bit_rate = m_config->videoBitrate * 1000;
        ctx->rc_max_rate = m_config->videoMaxrate * 1000;
        ctx->rc_buffer_size = m_config->videoBufsize * 1000;
        ctx->hw_frames_ctx = av_buffer_ref(m_hwFramesRef);
        if (!ctx->hw_frames_ctx) {
            LOG_ERROR("[gpu-video] av_buffer_ref(encoder hw_frames_ctx) failed");
            avcodec_free_context(&ctx);
            return false;
        }

        av_opt_set(ctx->priv_data, "async_depth", "1", 0);
        av_opt_set(ctx->priv_data, "low_power", lowPower ? "1" : "0", 0);
        av_opt_set(ctx->priv_data, "look_ahead", "0", 0);
        av_opt_set(ctx->priv_data, "low_delay_brc", "1", 0);
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->priv_data, "scenario", "displayremoting", 0);

        int ret = avcodec_open2(ctx, codec, nullptr);
        if (ret == 0) {
            m_codecCtx = ctx;
            LOG_INFO("[gpu-video] encoder opened: h264_qsv  pix_fmt=QSV  %dx%d  bitrate=%dkbps  "
                     "maxrate=%dkbps  bufsize=%dkbits  gop=%d  low_power=%d  look_ahead=0  low_delay_brc=1",
                     m_config->outputWidth, m_config->outputHeight,
                     m_config->videoBitrate, m_config->videoMaxrate,
                     m_config->videoBufsize, m_config->gopSize, lowPower);
            return true;
        }

        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("[gpu-video] h264_qsv low_power=%d failed: %s", lowPower, errBuf);
        avcodec_free_context(&ctx);
    }

    return false;
}

bool GpuVideoEncodeThread::encodeFrame(AVFrame* frame, int serial, int64_t pts) {
    frame->pts = pts;
    int ret = avcodec_send_frame(m_codecCtx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[gpu-video] avcodec_send_frame error: %s", errBuf);
        if (m_errorCallback) m_errorCallback(errBuf);
        return false;
    }
    // LOG_DEBUG("[gpu-video] send_frame pts=%lld (90kHz)", (long long)pts);
    return true;
}

void GpuVideoEncodeThread::receivePackets(int serial,
                                          int64_t beforeUs,
                                          int64_t* maxEncodeUs,
                                          int64_t* encodeCount) {
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return;

        int ret = avcodec_receive_packet(m_codecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            return;
        }
        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("[gpu-video] avcodec_receive_packet error: %s", errBuf);
            av_packet_free(&pkt);
            return;
        }

        int64_t nowUs = av_gettime_relative();
        int64_t encodeUs = nowUs - beforeUs;
        if (encodeUs > *maxEncodeUs) *maxEncodeUs = encodeUs;
        (*encodeCount)++;

        EncodedPacket ep;
        ep.pkt = pkt;
        ep.streamIndex = 0;
        ep.serial = serial;
        ep.timeBase = m_codecCtx->time_base;
        bool enqueued = m_outputQueue->push(std::move(ep));
        if (!enqueued) {
            m_stats->videoFramesDropped++;
        }

        m_stats->videoFramesEncoded++;
        m_stats->encodedQueueDepth = m_outputQueue->size();
    }
}

void GpuVideoEncodeThread::run() {
    LOG_INFO("[gpu-video] Thread started, serial=%d", m_serial.load());

    int64_t startUs = av_gettime_relative();
    int64_t lastStatsUs = startUs;
    int64_t frameCount = 0;
    int64_t encodeCount = 0;
    int64_t dropCount = 0;
    int64_t errorCount = 0;
    int64_t maxEncodeUs = 0;
    int64_t lastVideoPtsMs = -1;
    int serial = m_serial.load();

    int64_t serialStartUs = startUs;
    int64_t lastPts = -1;  // -1 = first frame, helper outputs pts=0

    while (!m_abort) {
        int currentSerial = m_serial.load();
        if (currentSerial != serial) {
            serial = currentSerial;
            serialStartUs = av_gettime_relative();
            lastPts = -1;
            avcodec_flush_buffers(m_codecCtx);
        }

        AVFrame* frame = m_pendingFrame;
        m_pendingFrame = nullptr;

        if (!frame) {
            frame = av_frame_alloc();
            if (!frame) {
                LOG_ERROR("[gpu-video] av_frame_alloc failed");
                break;
            }

            int ret = av_buffersink_get_frame(m_sinkCtx, frame);
            if (ret == AVERROR(EAGAIN)) {
                av_frame_free(&frame);
                av_usleep(1000);
                continue;
            }
            if (ret == AVERROR_EOF) {
                av_frame_free(&frame);
                LOG_WARN("[gpu-video] EOF from ddagrab filter graph");
                break;
            }
            if (ret < 0) {
                errorCount++;
                char errBuf[256];
                av_strerror(ret, errBuf, sizeof(errBuf));
                if (errorCount <= 5 || errorCount % 100 == 0) {
                    LOG_ERROR("[gpu-video] av_buffersink_get_frame error: %s (count=%lld)",
                              errBuf, (long long)errorCount);
                }
                av_frame_free(&frame);
                av_usleep(10000);
                continue;
            }
        }

        // Record capture wall-clock AFTER frame acquisition (not before blocking call)
        int64_t captureUs = av_gettime_relative();

        frameCount++;
        m_stats->videoFramesCaptured++;
        if (m_stats->firstVideoCaptureUs.load() == 0) {
            m_stats->firstVideoCaptureUs = captureUs;
        }

        int64_t videoPts = wallClockToVideoPts(captureUs, serialStartUs, lastPts);
        int64_t videoMs  = (captureUs - serialStartUs) / 1000LL;
        if (videoMs < lastVideoPtsMs && lastVideoPtsMs >= 0) {
            m_stats->ptsErrorCount++;
        }
        lastVideoPtsMs = videoMs;
        m_stats->videoPtsMs = videoMs;

        if (!encodeFrame(frame, serial, videoPts)) {
            dropCount++;
        }
        av_frame_free(&frame);

        receivePackets(serial, captureUs, &maxEncodeUs, &encodeCount);

        int64_t nowUs = av_gettime_relative();
        if (nowUs - lastStatsUs > 5000000) {
            double elapsedSec = (nowUs - startUs) / 1000000.0;
            double fps = elapsedSec > 0 ? encodeCount / elapsedSec : 0;
            m_stats->windowEncodeMaxUs = maxEncodeUs;
            m_stats->windowEncQueueMax = m_outputQueue->size();
            LOG_INFO("[gpu-video] fps=%.1f  captured=%lld  encoded=%lld  dropped=%lld  "
                     "errors=%lld  maxUs=%lld  encQ=%d",
                     fps, (long long)frameCount, (long long)encodeCount,
                     (long long)dropCount, (long long)errorCount,
                     (long long)maxEncodeUs, m_outputQueue->size());
            lastStatsUs = nowUs;
            maxEncodeUs = 0;
        }
    }

    avcodec_send_frame(m_codecCtx, nullptr);
    receivePackets(serial, av_gettime_relative(), &maxEncodeUs, &encodeCount);

    int64_t endUs = av_gettime_relative();
    double totalSec = (endUs - startUs) / 1000000.0;
    double avgFps = totalSec > 0 ? encodeCount / totalSec : 0;
    LOG_INFO("[gpu-video] Thread stopped: captured=%lld  encoded=%lld  dropped=%lld  "
             "errors=%lld  avgFps=%.1f  maxUs=%lld",
             (long long)frameCount, (long long)encodeCount, (long long)dropCount,
             (long long)errorCount, avgFps, (long long)maxEncodeUs);
}

void GpuVideoEncodeThread::close() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_hwFramesRef) {
        av_buffer_unref(&m_hwFramesRef);
    }
    if (m_pendingFrame) {
        av_frame_free(&m_pendingFrame);
    }
    if (m_filterGraph) {
        avfilter_graph_free(&m_filterGraph);
        m_sourceCtx = nullptr;
        m_hwmapCtx = nullptr;
        m_scaleCtx = nullptr;
        m_sinkCtx = nullptr;
    }
}
