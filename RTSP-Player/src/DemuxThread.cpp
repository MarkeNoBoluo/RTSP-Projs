#include "DemuxThread.h"
#include "PlayerStateMachine.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

DemuxThread::DemuxThread(PlayerStateMachine* sm, PlayerStats* stats,
                         PacketQueue* videoQueue, PacketQueue* audioQueue)
    : m_stateMachine(sm)
    , m_stats(stats)
    , m_videoQueue(videoQueue)
    , m_audioQueue(audioQueue)
{
}

DemuxThread::~DemuxThread() {
    stop();
    join();
}

void DemuxThread::prepareForOpen() {
    m_lastReadTime = av_gettime_relative();
    m_abort = false;
}

void DemuxThread::setContext(AVFormatContext* fmtCtx, AVStream* videoStream, AVStream* audioStream) {
    m_fmtCtx = fmtCtx;
    m_videoStream = videoStream;
    m_audioStream = audioStream;
    if (m_fmtCtx) {
        AVIOInterruptCB intrCb = { &interruptCallback, this };
        m_fmtCtx->interrupt_callback = intrCb;
    }
}

void DemuxThread::start() {
    m_abort = false;
    m_thread = std::thread(&DemuxThread::run, this);
}

void DemuxThread::stop() {
    m_abort = true;
}

void DemuxThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void DemuxThread::run() {
    LOG_INFO("Demux thread running");
    m_abort = false;
    m_lastReadTime = av_gettime_relative();

    if (!m_fmtCtx) {
        LOG_ERROR("DemuxThread: no format context");
        return;
    }

    m_stateMachine->transition(PlayerState::Connecting, PlayerState::Playing);

    int serial = m_serial.load(std::memory_order_acquire);
    AVPacket* pkt = av_packet_alloc();

    while (!m_abort) {
        m_lastReadTime = av_gettime_relative();
        int ret = av_read_frame(m_fmtCtx, pkt);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("av_read_frame returned EOF, triggering reconnect");
                if (m_onStreamError) m_onStreamError();
                break;
            }
            if (m_abort) break;
            char errbuf[256] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("av_read_frame error: %s (code=%d)", errbuf, ret);
            if (m_onStreamError) m_onStreamError();
            break;
        }

        int curSerial = m_serial.load(std::memory_order_acquire);
        // LOG_DEBUG("Demux pkt: stream=%d size=%d", pkt->stream_index, pkt->size);
        if (pkt->stream_index == m_videoStream->index) {
            m_videoQueue->push(pkt, curSerial);
        } else if (m_audioStream && pkt->stream_index == m_audioStream->index) {
            m_audioQueue->push(pkt, curSerial);
        }

        av_packet_unref(pkt);
    }

    LOG_INFO("Demux thread exiting");
    av_packet_free(&pkt);
}

int DemuxThread::interruptCallback(void* opaque) {
    auto* self = static_cast<DemuxThread*>(opaque);
    if (self->m_abort) return 1;
    int64_t now = av_gettime_relative();
    if (now - self->m_lastReadTime > 3000000LL) return 1;
    return 0;
}
