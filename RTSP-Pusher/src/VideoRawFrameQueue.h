#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>

extern "C" {
#include <libavutil/frame.h>
}

struct RawVideoFrame {
    AVFrame* frame = nullptr;
    int64_t  captureTimeUs = 0;
    int      frameIndex = 0;
    int      serial = 0;
};

class VideoRawFrameQueue {
public:
    explicit VideoRawFrameQueue(int maxSize = 4);
    ~VideoRawFrameQueue();

    // Push a frame; if queue is full, the oldest frame is dropped.
    // Returns false if the frame was dropped, true if enqueued.
    bool push(RawVideoFrame&& f);

    // Pop a frame, blocking up to timeoutUs microseconds.
    // Returns true if a frame was popped, false on timeout or abort.
    bool pop(RawVideoFrame& f, int timeoutUs = 100000);

    // Flush all frames, freeing their AVFrames.
    void flush();

    // Abort all waiters.
    void abort();
    void clearAbort();

    int  size() const;
    int  maxSize() const { return m_maxSize; }

private:
    void freeFrames();

    std::queue<RawVideoFrame> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    int m_maxSize;
    bool m_abort = false;
};
