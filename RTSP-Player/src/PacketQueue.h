#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#ifdef __cplusplus
}
#endif

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    void init(AVRational timeBase, int capacityMs = 200, const char* name = "",
              bool dropOnOverflow = false, bool keyframeAware = false);
    bool push(AVPacket* pkt, int serial = 0);
    bool pop(AVPacket* pkt, int timeoutMs);
    void flush();
    void abort();
    int  size();
    int  durationMs() const;

    // Returns true if a key frame was dropped since last check (clears flag)
    bool checkKeyFrameDropped();

    // Returns true if a GOP-level discontinuity occurred (entire queue dropped on overflow).
    // Consumer must flush the decoder before sending the next packet.
    bool consumeDiscontinuity();

    // Thread-safe: locks, reads peak values, resets to 0
    void drainPeak(int& outDurationMs, int& outSize);

    const char* name() const { return m_name.c_str(); }

private:
    struct PacketNode {
        AVPacket* pkt = nullptr;
        int64_t   durationUs = 0;
        int       serial = 0;
    };

    bool    m_initialized = false;
    int     m_capacityMs  = 200;
    bool    m_dropOnOverflow = false;
    double  m_timeBaseUs  = 0.0;
    std::string m_name;

    std::deque<PacketNode> m_queue;
    std::mutex             m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool>       m_abort{false};
    std::atomic<bool>       m_keyFrameDropped{false};
    bool                    m_keyframeAware = false;
    bool                    m_waitingForKeyframe = false;  // protected by m_mutex
    std::atomic<bool>       m_discontinuity{false};
    int64_t                 m_totalDurationUs = 0;
    int                     m_peakDurationMs = 0;
    int                     m_peakSize = 0;
};
