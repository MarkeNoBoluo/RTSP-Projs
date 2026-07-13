#include "EncodedPacketQueue.h"
#include "logger/Logger.h"

EncodedPacketQueue::EncodedPacketQueue(int maxSize)
    : m_maxSize(maxSize) {}

EncodedPacketQueue::~EncodedPacketQueue() {
    freePackets();
}

bool EncodedPacketQueue::push(EncodedPacket&& p) {
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool isVideo = (p.streamIndex == 0);
    const bool isKeyVideo = isVideo && p.pkt && ((p.pkt->flags & AV_PKT_FLAG_KEY) != 0);

    if (m_waitingForVideoKeyframeAfterDrop) {
        if (!isKeyVideo) {
            if (p.pkt) {
                av_packet_free(&p.pkt);
            }
            return false;
        }

        freePackets();
        m_waitingForVideoKeyframeAfterDrop = false;
        m_queue.push(std::move(p));
        m_cond.notify_one();
        LOG_WARN("[packet-queue] resumed at video keyframe after overflow");
        return true;
    }

    if ((int)m_queue.size() >= m_maxSize) {
        freePackets();

        if (!isKeyVideo) {
            m_waitingForVideoKeyframeAfterDrop = true;
            if (p.pkt) {
                av_packet_free(&p.pkt);
            }
            LOG_WARN("[packet-queue] overflow: dropped queued packets, waiting for next video keyframe");
            return false;
        }

        m_queue.push(std::move(p));
        m_cond.notify_one();
        LOG_WARN("[packet-queue] overflow: dropped queued packets, resumed with video keyframe");
        return true;
    }

    m_queue.push(std::move(p));
    m_cond.notify_one();
    return true;
}

bool EncodedPacketQueue::pop(EncodedPacket& p, int timeoutUs) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_abort) return false;

    if (m_queue.empty()) {
        if (timeoutUs < 0) {
            m_cond.wait(lock, [this] { return !m_queue.empty() || m_abort; });
        } else {
            auto timeout = std::chrono::microseconds(timeoutUs);
            m_cond.wait_for(lock, timeout, [this] { return !m_queue.empty() || m_abort; });
        }
    }

    if (m_abort || m_queue.empty()) return false;

    p = std::move(m_queue.front());
    m_queue.pop();
    return true;
}

void EncodedPacketQueue::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    freePackets();
}

void EncodedPacketQueue::abort() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = true;
    m_cond.notify_all();
}

void EncodedPacketQueue::clearAbort() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = false;
}

int EncodedPacketQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_queue.size();
}

void EncodedPacketQueue::freePackets() {
    while (!m_queue.empty()) {
        EncodedPacket& p = m_queue.front();
        if (p.pkt) {
            av_packet_free(&p.pkt);
        }
        m_queue.pop();
    }
}
