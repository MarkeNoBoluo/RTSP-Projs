#include "VideoFrameQueue.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/frame.h>
}

VideoFrameQueue::VideoFrameQueue() {
    for (int i = 0; i < kSlotCount; i++) {
        m_avFrames[i] = av_frame_alloc();
        m_slots[i].frame = m_avFrames[i];
        m_slots[i].serial = 0;
    }
}

VideoFrameQueue::~VideoFrameQueue() {
    for (int i = 0; i < kSlotCount; i++) {
        av_frame_free(&m_avFrames[i]);
    }
}

bool VideoFrameQueue::writeFrame(AVFrame* srcFrame, int64_t pts, int serial, PlayerStats* stats) {
    // Validate source frame — count malformed frames as write failures
    if (!srcFrame || !srcFrame->data[0] || srcFrame->width <= 0 || srcFrame->height <= 0) {
        if (stats) {
            stats->frameQueueWriteFailures.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_count.load(std::memory_order_relaxed) >= kSlotCount) {
        m_displayIdx = (m_displayIdx + 1) % kSlotCount;
        m_count.fetch_sub(1, std::memory_order_relaxed);
        if (stats) {
            stats->frameQueueOverwrites.fetch_add(1, std::memory_order_relaxed);
            stats->framesDropped.fetch_add(1, std::memory_order_relaxed);
        }
        LOG_DEBUG("VideoFrameQueue overwrite: replacing oldest pending frame");
    }

    int di = m_decodeIdx;

    av_frame_unref(m_avFrames[di]);
    av_frame_move_ref(m_avFrames[di], srcFrame);
    m_slots[di].pts = pts;
    m_slots[di].serial = serial;

    if (m_width != srcFrame->width || m_height != srcFrame->height) {
        m_width  = srcFrame->width;
        m_height = srcFrame->height;
    }

    m_renderIdx = di;
    m_decodeIdx = (di + 1) % kSlotCount;
    m_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool VideoFrameQueue::hasNewFrame() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count.load(std::memory_order_relaxed) > 0;
}

int64_t VideoFrameQueue::peekDisplayPts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_count.load(std::memory_order_relaxed) == 0) return -1;
    int di = m_displayIdx;
    if (!m_avFrames[di]->data[0]) return -1;
    return m_slots[di].pts;
}

int VideoFrameQueue::peekDisplaySerial() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_count.load(std::memory_order_relaxed) == 0) return -1;
    return m_slots[m_displayIdx].serial;
}

AVFrame* VideoFrameQueue::displayFrame() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_count.load(std::memory_order_relaxed) == 0) return nullptr;
    return m_avFrames[m_displayIdx];
}

void VideoFrameQueue::advanceDisplay() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_count.load(std::memory_order_relaxed) == 0) return;
    m_displayIdx = (m_displayIdx + 1) % kSlotCount;
    m_count.fetch_sub(1, std::memory_order_relaxed);
}

void VideoFrameQueue::discardAndAdvance() {
    advanceDisplay();
}

void VideoFrameQueue::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < kSlotCount; i++) {
        av_frame_unref(m_avFrames[i]);
        m_slots[i].serial = 0;
    }
    m_decodeIdx  = 0;
    m_renderIdx  = 0;
    m_displayIdx = 0;
    m_count.store(0, std::memory_order_relaxed);
    m_width  = 0;
    m_height = 0;
    LOG_INFO("VideoFrameQueue flushed");
}
