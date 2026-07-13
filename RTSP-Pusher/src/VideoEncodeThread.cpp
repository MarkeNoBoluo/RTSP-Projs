#include "VideoEncodeThread.h"
#include "HardwareEncoderDetector.h"
#include "VideoRawFrameQueue.h"
#include "EncodedPacketQueue.h"
#include "PtsUtils.h"
#include "logger/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

VideoEncodeThread::VideoEncodeThread(const PusherConfig* config,
                                     VideoRawFrameQueue* inputQueue,
                                     EncodedPacketQueue* outputQueue,
                                     PusherStats* stats)
    : m_config(config), m_inputQueue(inputQueue), m_outputQueue(outputQueue), m_stats(stats) {
    m_requestedEncoder = m_config->hwEncoder;
}

VideoEncodeThread::~VideoEncodeThread() {
    stop();
}

bool VideoEncodeThread::start(int serial) {
    m_serial = serial;
    m_abort = false;

    if (!openEncoder()) {
        LOG_ERROR("[encode] Failed to open encoder (requested: %s)", m_requestedEncoder);
        return false;
    }

    m_thread = std::thread(&VideoEncodeThread::run, this);
    return true;
}

void VideoEncodeThread::stop() {
    m_abort = true;
    if (m_inputQueue) m_inputQueue->abort();
}

void VideoEncodeThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    closeEncoder();
}

void VideoEncodeThread::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}

void VideoEncodeThread::setSerial(int serial) {
    m_serial = serial;
}

bool VideoEncodeThread::openEncoder() {
    const char* requestedName = resolveEncoderName(m_config->hwEncoder);

    // Defensive: guard against invalid/null config
    if (!requestedName) {
        LOG_ERROR("[encode] Invalid hw-encoder setting: %s",
                  m_config->hwEncoder ? m_config->hwEncoder : "(null)");
        return false;
    }

    bool isExplicit = (std::strcmp(m_config->hwEncoder, "off") != 0 &&
                       std::strcmp(m_config->hwEncoder, "auto") != 0);
    bool isAuto = (std::strcmp(m_config->hwEncoder, "auto") == 0);

    // Helper: set common codec context params shared by all encoders
    auto setCommonParams = [&](AVCodecContext* ctx) {
        ctx->width     = m_config->outputWidth;
        ctx->height    = m_config->outputHeight;
        ctx->time_base = {1, 90000};
        ctx->framerate = {m_config->captureFps, 1};
        ctx->pix_fmt   = m_encoderPixFmt;
        ctx->gop_size      = m_config->gopSize;
        ctx->max_b_frames  = 0;
        ctx->flags        |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags        |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ctx->bit_rate       = m_config->videoBitrate * 1000;
        ctx->rc_max_rate    = m_config->videoMaxrate * 1000;
        ctx->rc_buffer_size = m_config->videoBufsize * 1000;
    };

    // Helper: open a non-QSV encoder with fresh context
    auto tryOpenStandard = [&](const char* name, AVPixelFormat pixFmt) -> AVCodecContext* {
        const AVCodec* c = avcodec_find_encoder_by_name(name);
        if (!c) return nullptr;
        AVCodecContext* ctx = avcodec_alloc_context3(c);
        if (!ctx) return nullptr;
        m_encoderPixFmt = pixFmt;
        setCommonParams(ctx);

        // x264-specific options
        if (std::strcmp(name, "libx264") == 0) {
            av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
            av_opt_set(ctx->priv_data, "profile", "main", 0);
            av_opt_set(ctx->priv_data, "level", "5.0", 0);
            if (m_config->crf > 0) {
                av_opt_set(ctx->priv_data, "crf",
                           std::to_string(m_config->crf).c_str(), 0);
            }
            av_opt_set(ctx->priv_data, "x264-params",
                       "force-cfr=1:keyint=30:min-keyint=30:scenecut=0:sliced-threads=1", 0);
        }

        // NVENC-specific options
        if (std::strstr(name, "nvenc")) {
            av_opt_set(ctx->priv_data, "preset", "p4", 0);
            av_opt_set(ctx->priv_data, "tune", "ll", 0);
            av_opt_set(ctx->priv_data, "rc", "vbr", 0);
            av_opt_set(ctx->priv_data, "delay", "0", 0);
            av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
        }

        int ret = avcodec_open2(ctx, c, nullptr);
        if (ret < 0) {
            char eb[256];
            av_strerror(ret, eb, sizeof(eb));
            LOG_ERROR("[encode] %s avcodec_open2 failed: %s", name, eb);
            avcodec_free_context(&ctx);
            return nullptr;
        }
        return ctx;
    };

    // Helper: open QSV with low_power retry (fresh context each attempt)
    auto tryOpenQsv = [&]() -> AVCodecContext* {
        const AVCodec* c = avcodec_find_encoder_by_name("h264_qsv");
        if (!c) return nullptr;
        for (int lp = 1; lp >= 0; --lp) {
            AVCodecContext* ctx = avcodec_alloc_context3(c);
            if (!ctx) return nullptr;
            m_encoderPixFmt = AV_PIX_FMT_NV12;
            setCommonParams(ctx);
            av_opt_set(ctx->priv_data, "async_depth", "1", 0);
            char lpStr[2];
            snprintf(lpStr, sizeof(lpStr), "%d", lp);
            av_opt_set(ctx->priv_data, "low_power", lpStr, 0);
            av_opt_set(ctx->priv_data, "look_ahead", "0", 0);
            av_opt_set(ctx->priv_data, "low_delay_brc", "1", 0);
            av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(ctx->priv_data, "scenario", "displayremoting", 0);
            int ret = avcodec_open2(ctx, c, nullptr);
            if (ret == 0) return ctx;
            char eb[256];
            av_strerror(ret, eb, sizeof(eb));
            LOG_WARN("[encode] h264_qsv low_power=%d failed: %s", lp, eb);
            avcodec_free_context(&ctx);
        }
        return nullptr;
    };

    // ── Linear try chain ──
    m_codecCtx = nullptr;

    bool wantQsv = (std::strstr(requestedName, "qsv") != nullptr);
    bool wantSoftware = (std::strcmp(requestedName, "libx264") == 0);

    if (wantQsv) {
        m_codecCtx = tryOpenQsv();
        if (m_codecCtx) {
            m_encoderName = "h264_qsv";
        }
    } else {
        AVPixelFormat pf = AV_PIX_FMT_YUV420P;
        m_codecCtx = tryOpenStandard(requestedName, pf);
        if (m_codecCtx) {
            m_encoderName = requestedName;
        }
    }

    // ── Fallback: auto mode → NVENC → libx264 ──
    if (!m_codecCtx && isAuto) {
        if (wantQsv) {
            // QSV failed, try NVENC next
            LOG_WARN("[encode] h264_qsv unavailable, trying h264_nvenc");
            m_codecCtx = tryOpenStandard("h264_nvenc", AV_PIX_FMT_YUV420P);
            if (m_codecCtx) {
                m_encoderName = "h264_nvenc";
            }
        }
        if (!m_codecCtx && !wantSoftware) {
            LOG_WARN("[encode] %s unavailable, falling back to libx264", requestedName);
            m_codecCtx = tryOpenStandard("libx264", AV_PIX_FMT_YUV420P);
            if (m_codecCtx) {
                m_encoderName = "libx264";
            }
        }
    }

    // ── Final check ──
    if (!m_codecCtx) {
        if (isExplicit) {
            LOG_ERROR("[encode] %s encoder failed to open. "
                      "Use --hw-encoder auto for fallback.", requestedName);
        } else {
            LOG_ERROR("[encode] All encoder attempts failed");
        }
        return false;
    }

    // Allocate scaled frame
    m_scaledFrame = av_frame_alloc();
    if (!m_scaledFrame) {
        LOG_ERROR("[encode] av_frame_alloc failed");
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    m_scaledFrame->format = m_encoderPixFmt;
    m_scaledFrame->width  = m_codecCtx->width;
    m_scaledFrame->height = m_codecCtx->height;
    int ret = av_frame_get_buffer(m_scaledFrame, 0);
    if (ret < 0) {
        LOG_ERROR("[encode] av_frame_get_buffer failed");
        av_frame_free(&m_scaledFrame);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // Log actual encoder (authoritative)
    LOG_INFO("[encode] encoder opened: %s  pix_fmt=%s  %dx%d  bitrate=%dkbps  "
             "maxrate=%dkbps  bufsize=%dkbits  gop=%d  crf=%d",
             m_encoderName,
             m_encoderPixFmt == AV_PIX_FMT_NV12 ? "NV12" : "YUV420P",
             m_config->outputWidth, m_config->outputHeight,
             m_config->videoBitrate, m_config->videoMaxrate, m_config->videoBufsize,
             m_config->gopSize, m_config->crf);
    return true;
}

bool VideoEncodeThread::ensureSwsContext(int srcW, int srcH, int srcFmt) {
    if (m_swsCtx && srcW == m_swsSrcW && srcH == m_swsSrcH && srcFmt == m_swsSrcFmt) {
        return true; // already correct
    }

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    m_swsCtx = sws_getContext(srcW, srcH, (AVPixelFormat)srcFmt,
                              m_config->outputWidth, m_config->outputHeight,
                              m_encoderPixFmt,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        LOG_ERROR("[encode] sws_getContext failed: %dx%d fmt=%d -> %dx%d pixFmt=%d",
                  srcW, srcH, srcFmt, m_config->outputWidth, m_config->outputHeight,
                  (int)m_encoderPixFmt);
        return false;
    }

    m_swsSrcW = srcW;
    m_swsSrcH = srcH;
    m_swsSrcFmt = srcFmt;

    LOG_INFO("[encode] SwsContext created: %dx%d fmt=%d -> %dx%d pixFmt=%d",
             srcW, srcH, srcFmt, m_config->outputWidth, m_config->outputHeight,
             (int)m_encoderPixFmt);
    return true;
}

bool VideoEncodeThread::convertFrame(AVFrame* src, AVFrame* dst) {
    if (!ensureSwsContext(src->width, src->height, src->format)) {
        return false;
    }
    // Ensure dst buffer is not shared with encoder's internal references
    int writableRet = av_frame_make_writable(dst);
    if (writableRet < 0) {
        char errBuf[256];
        av_strerror(writableRet, errBuf, sizeof(errBuf));
        LOG_ERROR("[encode] av_frame_make_writable failed: %s", errBuf);
        return false;
    }
    int ret = sws_scale(m_swsCtx, src->data, src->linesize, 0, src->height,
                        dst->data, dst->linesize);
    if (ret != dst->height) {
        LOG_ERROR("[encode] sws_scale failed: expected %d, got %d", dst->height, ret);
        return false;
    }
    return true;
}

void VideoEncodeThread::run() {
    LOG_INFO("[encode] Thread started, serial=%d", m_serial.load());

    int64_t startUs = av_gettime_relative();
    int64_t lastStatsUs = startUs;
    int64_t encodeCount = 0;
    int64_t dropCount = 0;
    int64_t maxEncodeUs = 0;
    int64_t lastVideoPtsMs = -1;
    int serial = m_serial.load();

    // Defer serialStartUs init until first frame of this serial is popped
    int64_t serialStartUs = 0;
    int64_t lastPts = -1;
    bool needPtsReset = true;

    RawVideoFrame rawFrame;
    rawFrame.frame = nullptr;

    while (!m_abort) {
        // Check serial; discard and restart if changed
        int currentSerial = m_serial.load();
        if (currentSerial != serial) {
            serial = currentSerial;
            needPtsReset = true;
            // Flush encoder on serial change
            if (m_codecCtx) {
                avcodec_flush_buffers(m_codecCtx);
            }
        }

        // Pop raw frame from queue
        if (!m_inputQueue->pop(rawFrame, 100000)) {
            if (m_abort) break;
            continue;
        }

        // Check frame serial
        if (rawFrame.serial != serial) {
            if (rawFrame.frame) av_frame_free(&rawFrame.frame);
            dropCount++;
            continue;
        }

        AVFrame* srcFrame = rawFrame.frame;
        if (!srcFrame || srcFrame->width <= 0 || srcFrame->height <= 0) {
            if (srcFrame) av_frame_free(&srcFrame);
            dropCount++;
            continue;
        }

        // Initialize PTS baseline from first frame after start or serial change
        if (needPtsReset) {
            serialStartUs = rawFrame.captureTimeUs;
            lastPts = -1;
            needPtsReset = false;
        }

        // Convert BGRA → YUV420P with scaling
        int64_t beforeUs = av_gettime_relative();
        if (!convertFrame(srcFrame, m_scaledFrame)) {
            av_frame_free(&srcFrame);
            dropCount++;
            continue;
        }
        av_frame_free(&srcFrame);

        // Set PTS from wall-clock capture time (not frame index)
        int64_t videoPts = wallClockToVideoPts(rawFrame.captureTimeUs, serialStartUs, lastPts);
        m_scaledFrame->pts = videoPts;

        // Encode
        int ret = avcodec_send_frame(m_codecCtx, m_scaledFrame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                // Encoder full, skip this frame
                encodeCount++; // count as attempted
                continue;
            }
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("[encode] avcodec_send_frame error: %s", errBuf);
            if (m_errorCallback) m_errorCallback(errBuf);
            continue;
        }

        // LOG_DEBUG("[encode] send_frame pts=%lld (90kHz)", (long long)videoPts);

        // Receive encoded packets
        while (true) {
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) break;

            ret = avcodec_receive_packet(m_codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&pkt);
                break;
            }
            if (ret < 0) {
                char errBuf[256];
                av_strerror(ret, errBuf, sizeof(errBuf));
                LOG_ERROR("[encode] avcodec_receive_packet error: %s", errBuf);
                av_packet_free(&pkt);
                break;
            }

            int64_t nowUs = av_gettime_relative();
            int64_t encodeUs = nowUs - beforeUs;
            if (encodeUs > maxEncodeUs) {
                maxEncodeUs = encodeUs;
            }

            encodeCount++;

            // Record first video capture wall-clock
            if (m_stats->firstVideoCaptureUs.load() == 0) {
                m_stats->firstVideoCaptureUs = rawFrame.captureTimeUs;
            }

            // Compute video PTS in ms for A/V sync observation — use wall-clock
            int64_t videoMs = (rawFrame.captureTimeUs - serialStartUs) / 1000LL;
            if (videoMs < lastVideoPtsMs && lastVideoPtsMs >= 0) {
                m_stats->ptsErrorCount++;
            }
            lastVideoPtsMs = videoMs;
            m_stats->videoPtsMs = videoMs;

            EncodedPacket ep;
            ep.pkt = pkt;
            ep.streamIndex = 0; // video
            ep.serial = serial;
            ep.timeBase = m_codecCtx->time_base;

            bool enqueued = m_outputQueue->push(std::move(ep));
            if (!enqueued) {
                m_stats->videoFramesDropped++;
            }

            m_stats->videoFramesEncoded++;
            m_stats->encodedQueueDepth = m_outputQueue->size();
        }

        // Periodic stats
        int64_t nowUs = av_gettime_relative();
        if (nowUs - lastStatsUs > 5000000) {
            double elapsedSec = (nowUs - startUs) / 1000000.0;
            double fps = elapsedSec > 0 ? encodeCount / elapsedSec : 0;
            m_stats->windowEncodeMaxUs = maxEncodeUs;
            m_stats->windowEncQueueMax = m_outputQueue->size();
            LOG_INFO("[encode] fps=%.1f  encoded=%lld  dropped=%lld  maxUs=%lld  encQ=%d",
                     fps, (long long)encodeCount, (long long)dropCount,
                     (long long)maxEncodeUs, m_outputQueue->size());
            lastStatsUs = nowUs;
            maxEncodeUs = 0;
        }
    }

    int64_t endUs = av_gettime_relative();
    double totalSec = (endUs - startUs) / 1000000.0;
    double avgFps = totalSec > 0 ? encodeCount / totalSec : 0;
    LOG_INFO("[encode] Thread stopped: encoded=%lld  dropped=%lld  avgFps=%.1f  maxUs=%lld",
             (long long)encodeCount, (long long)dropCount, avgFps, (long long)maxEncodeUs);
}

void VideoEncodeThread::closeEncoder() {
    if (m_scaledFrame) {
        av_frame_free(&m_scaledFrame);
        m_scaledFrame = nullptr;
    }
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}
