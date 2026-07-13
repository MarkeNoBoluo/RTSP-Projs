#include "RTSPMuxThread.h"
#include "EncodedPacketQueue.h"
#include "logger/Logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

RTSPMuxThread::RTSPMuxThread(const PusherConfig* config,
                             EncodedPacketQueue* packetQueue,
                             PusherStats* stats)
    : m_config(config), m_packetQueue(packetQueue), m_stats(stats) {}

RTSPMuxThread::~RTSPMuxThread() {
    stop();
}

bool RTSPMuxThread::open(AVCodecContext* videoCodecCtx, AVCodecContext* audioCodecCtx) {
    int ret = avformat_alloc_output_context2(&m_outputCtx, nullptr, "rtsp",
                                             m_config->rtspUrl);
    if (ret < 0 || !m_outputCtx) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[mux] avformat_alloc_output_context2(rtsp) failed: %s", errBuf);
        return false;
    }

    // Set output options
    av_opt_set(m_outputCtx->priv_data, "rtsp_transport", m_config->rtspTransport, 0);
    av_opt_set(m_outputCtx->priv_data, "muxdelay", "0", 0);

    // Create video stream
    if (videoCodecCtx) {
        if (!createVideoStream(videoCodecCtx)) {
            avformat_free_context(m_outputCtx);
            m_outputCtx = nullptr;
            return false;
        }
    }

    // Create audio stream (Phase 5)
    if (audioCodecCtx) {
        m_audioStreamIndex = createAudioStream(audioCodecCtx);
        if (m_audioStreamIndex < 0) {
            LOG_WARN("[mux] Failed to create audio stream, continuing without audio");
        }
    }

    LOG_INFO("[mux] RTSP output created: %s  transport=%s",
             m_config->rtspUrl, m_config->rtspTransport);
    return true;
}

bool RTSPMuxThread::createVideoStream(AVCodecContext* codecCtx) {
    AVStream* stream = avformat_new_stream(m_outputCtx, nullptr);
    if (!stream) {
        LOG_ERROR("[mux] avformat_new_stream(video) failed");
        return false;
    }

    m_videoStreamIndex = stream->index;

    // Copy codec parameters
    int ret = avcodec_parameters_from_context(stream->codecpar, codecCtx);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[mux] avcodec_parameters_from_context(video) failed: %s", errBuf);
        return false;
    }

    stream->time_base = codecCtx->time_base;
    stream->avg_frame_rate = codecCtx->framerate;

    LOG_INFO("[mux] Video stream created: index=%d  time_base=%d/%d",
             m_videoStreamIndex, stream->time_base.num, stream->time_base.den);
    return true;
}

int RTSPMuxThread::createAudioStream(AVCodecContext* codecCtx) {
    AVStream* stream = avformat_new_stream(m_outputCtx, nullptr);
    if (!stream) {
        LOG_ERROR("[mux] avformat_new_stream(audio) failed");
        return -1;
    }

    int ret = avcodec_parameters_from_context(stream->codecpar, codecCtx);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[mux] avcodec_parameters_from_context(audio) failed: %s", errBuf);
        return -1;
    }

    stream->time_base = codecCtx->time_base;

    LOG_INFO("[mux] Audio stream created: index=%d  time_base=%d/%d",
             stream->index, stream->time_base.num, stream->time_base.den);
    return stream->index;
}

bool RTSPMuxThread::writeHeader() {
    if (!m_outputCtx) return false;

    // Debug: dump output format
    av_dump_format(m_outputCtx, 0, m_config->rtspUrl, 1);

    int ret = avformat_write_header(m_outputCtx, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[mux] avformat_write_header failed: %s", errBuf);
        return false;
    }

    m_headerWritten = true;
    LOG_INFO("[mux] RTSP header written successfully");
    return true;
}

bool RTSPMuxThread::start(int serial) {
    m_serial = serial;
    m_abort = false;

    if (!m_outputCtx) {
        LOG_ERROR("[mux] Cannot start: no output context");
        return false;
    }

    if (!writeHeader()) {
        return false;
    }

    m_thread = std::thread(&RTSPMuxThread::run, this);
    return true;
}

void RTSPMuxThread::stop() {
    m_abort = true;
    if (m_packetQueue) m_packetQueue->abort();
}

void RTSPMuxThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void RTSPMuxThread::close() {
    writeTrailer();
    if (m_outputCtx) {
        avformat_free_context(m_outputCtx);
        m_outputCtx = nullptr;
        m_videoStreamIndex = -1;
        m_audioStreamIndex = -1;
    }
}

void RTSPMuxThread::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}

void RTSPMuxThread::setSerial(int serial) {
    m_serial = serial;
}

void RTSPMuxThread::run() {
    LOG_INFO("[mux] Thread started, serial=%d", m_serial.load());

    int64_t startUs = av_gettime_relative();
    int64_t lastStatsUs = startUs;
    int64_t packetsWritten = 0;
    int64_t videoPackets = 0;
    int64_t audioPackets = 0;
    int64_t writeErrors = 0;
    int64_t maxWriteUs = 0;
    int64_t windowBytes = 0;    // accumulated packet size for bitrate calc
    int serial = m_serial.load();

    EncodedPacket ep;
    ep.pkt = nullptr;

    while (!m_abort) {
        int currentSerial = m_serial.load();
        if (currentSerial != serial) {
            serial = currentSerial;
            LOG_INFO("[mux] Serial changed to %d", serial);
        }

        if (!m_packetQueue->pop(ep, 100000)) {
            if (m_abort) break;
            continue;
        }

        // Discard packets from old serial
        if (ep.serial != serial || !ep.pkt) {
            if (ep.pkt) av_packet_free(&ep.pkt);
            continue;
        }

        AVPacket* pkt = ep.pkt;

        // Determine which stream
        int streamIndex = m_videoStreamIndex;
        if (ep.streamIndex == 1 && m_audioStreamIndex >= 0) {
            streamIndex = m_audioStreamIndex;
        }

        AVStream* stream = m_outputCtx->streams[streamIndex];
        pkt->stream_index = streamIndex;

        // Rescale PTS from encoder time_base to output stream time_base.
        // After avformat_write_header, the RTSP/RTP muxer sets
        // stream->time_base to {1, 90000} (90 kHz clock), while the
        // encoder uses {1, fps}.  Using stream->time_base as both src
        // and dst was a no-op — VLC received correct data but wrong
        // RTP timestamps, causing playback stutter / frame drop.
        av_packet_rescale_ts(pkt, ep.timeBase, stream->time_base);

        int64_t beforeUs = av_gettime_relative();

        // Save pkt->size before av_interleaved_write_frame() takes ownership
        int pktSize = pkt->size;

        int ret = av_interleaved_write_frame(m_outputCtx, pkt);
        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOG_ERROR("[mux] av_interleaved_write_frame error: %s (count=%lld)",
                     errBuf, (long long)writeErrors + 1);
            writeErrors++;
            m_stats->writeErrorCount++;
            av_packet_free(&ep.pkt);

            // Trigger error callback for lifecycle recovery
            if (m_errorCallback) {
                m_errorCallback(errBuf);
            }
            break;
        }

        int64_t afterUs = av_gettime_relative();
        int64_t writeUs = afterUs - beforeUs;
        if (writeUs > maxWriteUs) maxWriteUs = writeUs;

        packetsWritten++;
        windowBytes += pktSize;

        if (ep.streamIndex == 0) {
            videoPackets++;
            m_stats->videoPacketCount++;
        } else {
            audioPackets++;
            m_stats->audioPacketCount++;
        }
        m_stats->packetsWritten++;
        m_stats->encodedQueueDepth = m_packetQueue->size();

        av_packet_free(&ep.pkt);

        // Periodic stats
        if (afterUs - lastStatsUs > 5000000) {
            double elapsedSec = (afterUs - startUs) / 1000000.0;
            double fps = elapsedSec > 0 ? packetsWritten / elapsedSec : 0;

            // Window stats: bitrate from accumulated bytes
            double windowSec = (afterUs - lastStatsUs) / 1000000.0;
            if (windowSec > 0) {
                m_stats->bitrateKbps = (int)((windowBytes * 8) / (windowSec * 1000));
            }
            m_stats->windowMuxWriteMaxUs = maxWriteUs;
            m_stats->windowEncQueueMax = m_packetQueue->size();

            LOG_INFO("[mux] fps=%.1f  packets=%lld(v=%lld,a=%lld)  errors=%lld  "
                     "maxWriteUs=%lld  bitrateKbps=%d  queue=%d",
                     fps, (long long)packetsWritten,
                     (long long)videoPackets, (long long)audioPackets,
                     (long long)writeErrors,
                     (long long)maxWriteUs,
                     m_stats->bitrateKbps.load(),
                     m_packetQueue->size());
            lastStatsUs = afterUs;
            windowBytes = 0;
            maxWriteUs = 0;
        }
    }

    int64_t endUs = av_gettime_relative();
    LOG_INFO("[mux] Thread stopped: packets=%lld(v=%lld,a=%lld)  errors=%lld  duration=%.1fs",
             (long long)packetsWritten, (long long)videoPackets, (long long)audioPackets,
             (long long)writeErrors,
             (endUs - startUs) / 1000000.0);
}

void RTSPMuxThread::writeTrailer() {
    if (m_outputCtx && m_headerWritten) {
        av_write_trailer(m_outputCtx);
        m_headerWritten = false;
        LOG_INFO("[mux] RTSP trailer written");
    }
}
