#pragma once

#include "Common.h"
#include <atomic>
#include <cstdint>
#include <mutex>

class PlayerStats;

class VideoFrameQueue {
public:
    static constexpr int kSlotCount = 24;  // ~800ms buffer at 30fps, absorbs bursty decode
    VideoFrameQueue();
    ~VideoFrameQueue();

    bool writeFrame(AVFrame* srcFrame, int64_t pts, int serial, class PlayerStats* stats = nullptr);

    bool hasNewFrame() const;
    int count() const { return m_count.load(std::memory_order_acquire); }
    int64_t peekDisplayPts() const;
    int peekDisplaySerial() const;
    AVFrame* displayFrame();

    void advanceDisplay();
    void discardAndAdvance();

    void notifyAll() {}  // 预留，当前主循环用 polling
    void flush();

private:
    AVFrame*          m_avFrames[kSlotCount];
    VideoFrame        m_slots[kSlotCount];

    int m_decodeIdx{0};
    int m_renderIdx{0};
    int m_displayIdx{0};
    std::atomic<int> m_count{0};

    mutable std::mutex m_mutex;

    int m_width{0};
    int m_height{0};
};
