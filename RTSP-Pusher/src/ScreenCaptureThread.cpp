#include "ScreenCaptureThread.h"
#include "VideoRawFrameQueue.h"
#include "logger/Logger.h"
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

ScreenCaptureThread::ScreenCaptureThread(const PusherConfig* config,
                                         VideoRawFrameQueue* queue,
                                         PusherStats* stats)
    : m_config(config), m_queue(queue), m_stats(stats) {}

ScreenCaptureThread::~ScreenCaptureThread() {
    stop();
}

bool ScreenCaptureThread::start(int serial) {
    m_serial = serial;
    m_abort = false;

    if (!openInput()) {
        LOG_ERROR("[capture] Failed to open gdigrab input");
        return false;
    }

    m_thread = std::thread(&ScreenCaptureThread::run, this);
    return true;
}

void ScreenCaptureThread::stop() {
    m_abort = true;
    if (m_queue) m_queue->abort();
}

void ScreenCaptureThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    closeInput();
}

void ScreenCaptureThread::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}

void ScreenCaptureThread::setSerial(int serial) {
    m_serial = serial;
}

bool ScreenCaptureThread::openInput() {
    avdevice_register_all();

#if LIBAVFORMAT_VERSION_MAJOR >= 60
    const AVInputFormat* inputFormat = av_find_input_format("gdigrab");
#else
    AVInputFormat* inputFormat = av_find_input_format("gdigrab");
#endif
    if (!inputFormat) {
        LOG_ERROR("[capture] gdigrab input format not found");
        return false;
    }

    m_inputCtx = avformat_alloc_context();
    if (!m_inputCtx) {
        LOG_ERROR("[capture] avformat_alloc_context failed");
        return false;
    }

    AVDictionary* opts = nullptr;

    char framerateStr[32];
    snprintf(framerateStr, sizeof(framerateStr), "%d", m_config->captureFps);
    av_dict_set(&opts, "framerate", framerateStr, 0);

    char videoSizeStr[64];
    snprintf(videoSizeStr, sizeof(videoSizeStr), "%dx%d",
             m_config->captureWidth, m_config->captureHeight);
    av_dict_set(&opts, "video_size", videoSizeStr, 0);

    av_dict_set(&opts, "draw_mouse", "1", 0);

    // When capturing a specific monitor, set offset to crop the virtual desktop
    if (m_config->screenIndex > 0) {
        char offsetXStr[32];
        snprintf(offsetXStr, sizeof(offsetXStr), "%d", m_config->captureOffsetX);
        av_dict_set(&opts, "offset_x", offsetXStr, 0);

        char offsetYStr[32];
        snprintf(offsetYStr, sizeof(offsetYStr), "%d", m_config->captureOffsetY);
        av_dict_set(&opts, "offset_y", offsetYStr, 0);

        LOG_INFO("[capture] Screen offset: (%d, %d)",
                 m_config->captureOffsetX, m_config->captureOffsetY);
    }

    // Set probe size and analyzeduration for faster startup
    m_inputCtx->probesize = 32;
    m_inputCtx->max_analyze_duration = 1;

    int ret = avformat_open_input(&m_inputCtx, "desktop", inputFormat, &opts);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[capture] avformat_open_input(desktop) failed: %s", errBuf);
        av_dict_free(&opts);
        avformat_free_context(m_inputCtx);
        m_inputCtx = nullptr;
        return false;
    }
    av_dict_free(&opts);

    ret = avformat_find_stream_info(m_inputCtx, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[capture] avformat_find_stream_info failed: %s", errBuf);
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        return false;
    }

    m_videoStreamIndex = -1;
    for (unsigned i = 0; i < m_inputCtx->nb_streams; ++i) {
        if (m_inputCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;

            AVCodecParameters* par = m_inputCtx->streams[i]->codecpar;
            LOG_INFO("[capture] Input opened: %dx%d  pix_fmt=%d  stream=%d",
                     par->width, par->height, par->format, i);

            if (par->width <= 0 || par->height <= 0 || par->format < 0) {
                LOG_ERROR("[capture] Invalid video stream parameters: %dx%d pix_fmt=%d",
                          par->width, par->height, par->format);
                avformat_close_input(&m_inputCtx);
                m_inputCtx = nullptr;
                m_videoStreamIndex = -1;
                return false;
            }
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        LOG_ERROR("[capture] No video stream found in gdigrab input");
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        return false;
    }

    AVCodecParameters* par = m_inputCtx->streams[m_videoStreamIndex]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
    if (!decoder) {
        LOG_ERROR("[capture] Decoder not found for codec_id=%d", par->codec_id);
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        m_videoStreamIndex = -1;
        return false;
    }

    m_decoderCtx = avcodec_alloc_context3(decoder);
    if (!m_decoderCtx) {
        LOG_ERROR("[capture] avcodec_alloc_context3(decoder) failed");
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        m_videoStreamIndex = -1;
        return false;
    }

    ret = avcodec_parameters_to_context(m_decoderCtx, par);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[capture] avcodec_parameters_to_context failed: %s", errBuf);
        avcodec_free_context(&m_decoderCtx);
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        m_videoStreamIndex = -1;
        return false;
    }

    ret = avcodec_open2(m_decoderCtx, decoder, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[capture] avcodec_open2(decoder) failed: %s", errBuf);
        avcodec_free_context(&m_decoderCtx);
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        m_videoStreamIndex = -1;
        return false;
    }

    LOG_INFO("[capture] Decoder opened: codec_id=%d  output_fmt=%d",
             par->codec_id, m_decoderCtx->pix_fmt);

    LOG_INFO("[capture] gdigrab input opened successfully");
    return true;
}

void ScreenCaptureThread::run() {
    LOG_INFO("[capture] Thread started, serial=%d", m_serial.load());

    int64_t startUs = av_gettime_relative();
    int64_t lastStatsUs = startUs;
    int64_t frameCount = 0;
    int64_t errorCount = 0;
    int localRawQueueMax = 0;
    int serial = m_serial.load();

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        LOG_ERROR("[capture] av_packet_alloc failed");
        return;
    }

    while (!m_abort) {

        int ret = av_read_frame(m_inputCtx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_WARN("[capture] EOF from gdigrab");
                break;
            }
            errorCount++;
            if (errorCount <= 5 || errorCount % 100 == 0) {
                char errBuf[256];
                av_strerror(ret, errBuf, sizeof(errBuf));
                LOG_ERROR("[capture] av_read_frame error: %s (count=%lld)",
                         errBuf, (long long)errorCount);
            }
            av_packet_unref(pkt);
            av_usleep(10000);
            if (m_abort) break;
            continue;
        }

        if (pkt->stream_index != m_videoStreamIndex) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(m_decoderCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            errorCount++;
            if (errorCount <= 5 || errorCount % 100 == 0) {
                char errBuf[256];
                av_strerror(ret, errBuf, sizeof(errBuf));
                LOG_ERROR("[capture] avcodec_send_packet error: %s (count=%lld)",
                          errBuf, (long long)errorCount);
            }
            continue;
        }

        while (!m_abort) {
            AVFrame* decodedFrame = av_frame_alloc();
            if (!decodedFrame) {
                break;
            }

            ret = avcodec_receive_frame(m_decoderCtx, decodedFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_frame_free(&decodedFrame);
                break;
            }
            if (ret < 0) {
                errorCount++;
                if (errorCount <= 5 || errorCount % 100 == 0) {
                    char errBuf[256];
                    av_strerror(ret, errBuf, sizeof(errBuf));
                    LOG_ERROR("[capture] avcodec_receive_frame error: %s (count=%lld)",
                              errBuf, (long long)errorCount);
                }
                av_frame_free(&decodedFrame);
                break;
            }

            if (decodedFrame->width <= 0 || decodedFrame->height <= 0 ||
                decodedFrame->format < 0) {
                LOG_WARN("[capture] Dropping invalid decoded frame: %dx%d fmt=%d",
                         decodedFrame->width, decodedFrame->height, decodedFrame->format);
                av_frame_free(&decodedFrame);
                continue;
            }

            // Check serial; discard frame from old session
            int currentSerial = m_serial.load();
            if (currentSerial != serial) {
                serial = currentSerial;
                av_frame_free(&decodedFrame);
                continue;
            }

            int64_t nowUs = av_gettime_relative();
            frameCount++;

            RawVideoFrame rf;
            rf.frame = decodedFrame;
            rf.captureTimeUs = nowUs;
            rf.frameIndex = (int)frameCount;
            rf.serial = serial;

            bool enqueued = m_queue->push(std::move(rf));
            if (!enqueued) {
                m_stats->videoFramesDropped++;
            }

            m_stats->videoFramesCaptured++;
            int qs = m_queue->size();
            if (qs > localRawQueueMax) localRawQueueMax = qs;
            m_stats->videoRawQueueDepth = qs;

            // Periodic stats
            if (nowUs - lastStatsUs > 5000000) { // every 5 seconds
                double elapsedSec = (nowUs - startUs) / 1000000.0;
                double fps = elapsedSec > 0 ? frameCount / elapsedSec : 0;
                m_stats->windowRawQueueMax = localRawQueueMax;
                LOG_INFO("[capture] fps=%.1f  frames=%lld  errors=%lld  rawQ=%d  rawQMax=%d",
                         fps, (long long)frameCount, (long long)errorCount,
                         m_queue->size(), localRawQueueMax);
                lastStatsUs = nowUs;
                localRawQueueMax = 0;
            }
        }
    }

    av_packet_free(&pkt);

    int64_t endUs = av_gettime_relative();
    double totalSec = (endUs - startUs) / 1000000.0;
    double avgFps = totalSec > 0 ? frameCount / totalSec : 0;
    LOG_INFO("[capture] Thread stopped: frames=%lld  errors=%lld  avgFps=%.1f  duration=%.1fs",
             (long long)frameCount, (long long)errorCount, avgFps, totalSec);
}

void ScreenCaptureThread::closeInput() {
    if (m_decoderCtx) {
        avcodec_free_context(&m_decoderCtx);
        m_decoderCtx = nullptr;
    }
    if (m_inputCtx) {
        avformat_close_input(&m_inputCtx);
        m_inputCtx = nullptr;
        m_videoStreamIndex = -1;
    }
}
