#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

struct EncodedPacket {
    AVPacket* pkt = nullptr;
    int streamIndex = 0; // 0 = video, 1 = audio
    int serial = 0;
    AVRational timeBase{1, 90000}; // encoder time_base; used to rescale PTS before mux
};

class EncodedPacketQueue {
public:
    explicit EncodedPacketQueue(int maxSize = 60);
    ~EncodedPacketQueue();

    bool push(EncodedPacket&& p);
    bool pop(EncodedPacket& p, int timeoutUs = 100000);

    void flush();
    void abort();
    void clearAbort();

    int  size() const;
    int  maxSize() const { return m_maxSize; }

private:
    void freePackets();

    std::queue<EncodedPacket> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    int m_maxSize;
    bool m_abort = false;
    bool m_waitingForVideoKeyframeAfterDrop = false;
};
