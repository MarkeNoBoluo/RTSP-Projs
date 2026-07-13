#include "VideoRawFrameQueue.h"
#include <cstdint>

VideoRawFrameQueue::VideoRawFrameQueue(int maxSize)
    : m_maxSize(maxSize) {}

VideoRawFrameQueue::~VideoRawFrameQueue() {
    freeFrames();
}

bool VideoRawFrameQueue::push(RawVideoFrame&& f) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if ((int)m_queue.size() >= m_maxSize) {
        // Drop oldest frame
        RawVideoFrame& old = m_queue.front();
        if (old.frame) {
            av_frame_free(&old.frame);
        }
        m_queue.pop();
        m_cond.notify_one();
        // Return false to indicate a drop happened
        m_queue.push(std::move(f));
        return false;
    }

    m_queue.push(std::move(f));
    m_cond.notify_one();
    return true;
}

bool VideoRawFrameQueue::pop(RawVideoFrame& f, int timeoutUs) {
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

    f = std::move(m_queue.front());
    m_queue.pop();
    return true;
}

void VideoRawFrameQueue::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    freeFrames();
}

void VideoRawFrameQueue::abort() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = true;
    m_cond.notify_all();
}

void VideoRawFrameQueue::clearAbort() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = false;
}

int VideoRawFrameQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_queue.size();
}

void VideoRawFrameQueue::freeFrames() {
    while (!m_queue.empty()) {
        RawVideoFrame& f = m_queue.front();
        if (f.frame) {
            av_frame_free(&f.frame);
        }
        m_queue.pop();
    }
}
