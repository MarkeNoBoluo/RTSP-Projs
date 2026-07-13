#include "VideoDecodeThread.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

VideoDecodeThread::VideoDecodeThread(AVCodecContext* codecCtx, AVRational timeBase,
                                     PacketQueue* queue, VideoFrameQueue* frameQueue,
                                     PlayerStats* stats)
    : m_codecCtx(codecCtx)
    , m_timeBase(timeBase.num > 0 ? timeBase : AVRational{1, 90000})
    , m_queue(queue)
    , m_frameQueue(frameQueue)
    , m_stats(stats)
{
}

VideoDecodeThread::~VideoDecodeThread() {
    stop();
    join();
}

void VideoDecodeThread::start() {
    m_thread = std::thread(&VideoDecodeThread::run, this);
}

void VideoDecodeThread::stop() {
    m_abort = true;
}

void VideoDecodeThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

static bool isHwPixelFormat(int format) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

void VideoDecodeThread::run() {
    m_abort = false;
    m_ready = false;

    LOG_INFO("Video decode thread running");

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    AVFrame*  swFrame = av_frame_alloc();

    int timeoutCount = 0;
    bool wasStalled = false;
    int64_t stallBeginUs = 0;
    int curSerial = m_serial.load(std::memory_order_acquire);

    int64_t sendWallUs = 0, receiveWallUs = 0;
    int sendErrs = 0, recvErrs = 0;
    int slowSendCount = 0, slowRecvCount = 0;

    m_ready.store(true, std::memory_order_release);

    while (!m_abort) {
        if (!m_queue->pop(pkt, 100)) {
            timeoutCount++;
            if (timeoutCount <= 5) {
                LOG_DEBUG("VideoDecode: pop timeout #%d", timeoutCount);
            }
            if (timeoutCount == 10 && !wasStalled) {
                wasStalled = true;
                stallBeginUs = av_gettime_relative();
                m_stats->videoPopTimeouts++;
                LOG_INFO("VideoDecode: video stall begin (10 consecutive pop timeouts)");
            }
            if (timeoutCount == 30) {
                LOG_INFO("VideoDecode: queue empty, flushing decoder");
                avcodec_send_packet(m_codecCtx, nullptr);
                while (true) {
                    int flushRet = avcodec_receive_frame(m_codecCtx, frame);
                    if (flushRet == AVERROR(EAGAIN) || flushRet == AVERROR_EOF) break;
                    if (flushRet < 0) break;
                    AVFrame* outputFrame = frame;
                    if (isHwPixelFormat(frame->format)) {
                        int xferRet = av_hwframe_transfer_data(swFrame, frame, 0);
                        if (xferRet < 0) {
                            m_stats->hwTransferFailures++;
                            continue;
                        }
                        m_stats->hwDecodedFrames++;
                        av_frame_copy_props(swFrame, frame);
                        outputFrame = swFrame;
                    }
                    int64_t pts = outputFrame->pts;
                    if (pts == AV_NOPTS_VALUE) pts = outputFrame->pkt_dts;
                    if (pts != AV_NOPTS_VALUE) {
                        pts = av_rescale_q(pts, m_timeBase, AVRational{1, AV_TIME_BASE});
                    }
                    m_frameQueue->writeFrame(outputFrame, pts, curSerial, m_stats);
                    m_stats->framesDecoded++;
                }
                avcodec_flush_buffers(m_codecCtx);
            }
            continue;
        }

        if (wasStalled && timeoutCount >= 10) {
            int64_t stallDurationMs = (av_gettime_relative() - stallBeginUs) / 1000;
            LOG_INFO("VideoDecode: video stall end — %d consecutive timeouts, duration=%lldms",
                     timeoutCount, (long long)stallDurationMs);
            m_stats->videoStallCount++;
            wasStalled = false;
            stallBeginUs = 0;
        }
        timeoutCount = 0;

        int newSerial = m_serial.load(std::memory_order_acquire);
        if (newSerial != curSerial) {
            curSerial = newSerial;
            avcodec_flush_buffers(m_codecCtx);
            LOG_INFO("VideoDecode: serial changed to %d, flushing decoder", curSerial);
        }

        // If queue overflow caused GOP drop, flush decoder to start clean from next IDR
        if (m_queue->consumeDiscontinuity()) {
            LOG_WARN("VideoDecode: stream discontinuity, flushing decoder");
            avcodec_flush_buffers(m_codecCtx);
        }

        int64_t sendBefore = av_gettime_relative();
        int ret = avcodec_send_packet(m_codecCtx, pkt);
        int64_t sendUs = av_gettime_relative() - sendBefore;
        av_packet_unref(pkt);

        if (ret < 0) {
            sendErrs++;
            continue;
        }

        sendWallUs += sendUs;
        {
            int64_t curMax = m_stats->decodeSendUsMax.load(std::memory_order_acquire);
            while (sendUs > curMax) {
                if (m_stats->decodeSendUsMax.compare_exchange_weak(
                        curMax, sendUs,
                        std::memory_order_release, std::memory_order_acquire)) {
                    break;
                }
            }
        }
        if (sendUs > 50000) {
            slowSendCount++;
            if (slowSendCount <= 3 || slowSendCount % 10 == 0) {
                LOG_WARN("VideoDecode: send_packet slow (%lldus) count=%d",
                         (long long)sendUs, slowSendCount);
            }
        }

        curSerial = m_serial.load(std::memory_order_acquire);

        while (true) {
            int64_t recvBefore = av_gettime_relative();
            ret = avcodec_receive_frame(m_codecCtx, frame);
            int64_t recvUs = av_gettime_relative() - recvBefore;

            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) break;
            if (ret < 0) {
                recvErrs++;
                if (recvErrs <= 10) {
                    LOG_WARN("VideoDecode: receive_frame error %d (count=%d)", ret, recvErrs);
                }
                break;
            }

            // Track peak decode time
            receiveWallUs += recvUs;
            {
                int64_t curMax = m_stats->decodeReceiveUsMax.load(std::memory_order_acquire);
                while (recvUs > curMax) {
                    if (m_stats->decodeReceiveUsMax.compare_exchange_weak(
                            curMax, recvUs,
                            std::memory_order_release, std::memory_order_acquire)) {
                        break;
                    }
                }
            }
            if (recvUs > 50000) {
                slowRecvCount++;
                if (slowRecvCount <= 5 || slowRecvCount % 20 == 0) {
                    LOG_WARN("VideoDecode: receive_frame slow (%lldus) count=%d",
                             (long long)recvUs, slowRecvCount);
                }
            }

            AVFrame* outputFrame = frame;
            if (isHwPixelFormat(frame->format)) {
                int64_t xferBefore = av_gettime_relative();
                int xferRet = av_hwframe_transfer_data(swFrame, frame, 0);
                int64_t xferUs = av_gettime_relative() - xferBefore;
                if (xferRet < 0) {
                    m_stats->hwTransferFailures++;
                    continue;
                }
                {
                    int64_t curMax = m_stats->hwTransferMaxUs.load(std::memory_order_acquire);
                    while (xferUs > curMax) {
                        if (m_stats->hwTransferMaxUs.compare_exchange_weak(
                                curMax, xferUs,
                                std::memory_order_release, std::memory_order_acquire)) {
                            break;
                        }
                    }
                }
                m_stats->hwDecodedFrames++;
                av_frame_copy_props(swFrame, frame);
                outputFrame = swFrame;
            }

            int64_t pts = outputFrame->pts;
            if (pts == AV_NOPTS_VALUE) pts = outputFrame->pkt_dts;
            if (pts != AV_NOPTS_VALUE) {
                pts = av_rescale_q(pts, m_timeBase, AVRational{1, AV_TIME_BASE});
            }

            if (m_stats->framesDecoded <= 1) {
                LOG_INFO("Decoded frame #%lld: pts=%lld, %dx%d, fmt=%d",
                         (long long)m_stats->framesDecoded.load(), (long long)pts,
                         outputFrame->width, outputFrame->height, outputFrame->format);
            }

            m_frameQueue->writeFrame(outputFrame, pts, curSerial, m_stats);

            int64_t expectedZero = 0;
            m_stats->videoFirstDecodeUs.compare_exchange_strong(
                expectedZero, av_gettime_relative(),
                std::memory_order_release, std::memory_order_acquire);

            m_stats->framesDecoded++;
        }
    }

    av_frame_free(&frame);
    av_frame_free(&swFrame);
    av_packet_free(&pkt);

    m_stats->decodeErrorCount.store(sendErrs + recvErrs, std::memory_order_release);

    LOG_INFO("Video decode thread exiting: decoded=%lld sendErrs=%d recvErrs=%d "
             "slowSend=%d slowRecv=%d sendMax=%lldus recvMax=%lldus",
             (long long)m_stats->framesDecoded.load(),
             sendErrs, recvErrs, slowSendCount, slowRecvCount,
             (long long)m_stats->decodeSendUsMax.load(),
             (long long)m_stats->decodeReceiveUsMax.load());
}
