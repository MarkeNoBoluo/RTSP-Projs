#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

struct PusherStats {
    // Video capture
    std::atomic<int64_t> videoFramesCaptured{0};
    std::atomic<int64_t> videoFramesDropped{0};
    std::atomic<int64_t> videoFramesEncoded{0};

    // Audio capture
    std::atomic<int64_t> audioBytesCaptured{0};
    std::atomic<int64_t> audioFramesEncoded{0};

    // RTSP mux — per-packet-type counts
    std::atomic<int64_t> videoPacketCount{0};
    std::atomic<int64_t> audioPacketCount{0};
    std::atomic<int64_t> packetsWritten{0};
    std::atomic<int64_t> writeErrorCount{0};
    std::atomic<int64_t> reconnectCount{0};

    // Queue depth snapshots (current)
    std::atomic<int> videoRawQueueDepth{0};
    std::atomic<int> encodedQueueDepth{0};

    // Window-based peak queue depths (reset each 5s stats window)
    std::atomic<int> windowRawQueueMax{0};
    std::atomic<int> windowEncQueueMax{0};

    // Performance — window-based (reset each 5s stats window)
    std::atomic<int>     bitrateKbps{0};          // computed from mux bytes over 5s window
    std::atomic<int64_t> windowEncodeMaxUs{0};
    std::atomic<int64_t> windowMuxWriteMaxUs{0};

    // VBV underflow — counted from FFmpeg log callback
    std::atomic<int64_t> vbvUnderflowCount{0};

    // Audio ring buffer health
    std::atomic<int>     audioRingBytes{0};       // current readable bytes
    std::atomic<int64_t> audioOverflowCount{0};   // write() drops (buffer full)
    std::atomic<int64_t> audioUnderrunCount{0};   // audio frame waits beyond threshold

    // A/V sync observation (Stage 3)
    std::atomic<int64_t> pipelineStartUs{0};      // set at pipeline start for PTS reference
    std::atomic<int64_t> firstVideoCaptureUs{0};  // wall-clock of first encoded video frame
    std::atomic<int64_t> firstAudioCaptureUs{0};  // wall-clock of first encoded audio packet
    std::atomic<int64_t> videoPtsMs{0};           // latest video PTS in ms (media time)
    std::atomic<int64_t> audioPtsMs{0};           // latest audio PTS in ms (media time)
    std::atomic<int64_t> avOffsetMs{0};           // audioPtsMs - videoPtsMs
    std::atomic<int64_t> ptsErrorCount{0};        // non-monotonic or anomalous PTS/DTS

    void reset();
    void writeCsvHeader(FILE* f);
    void writeCsvRow(FILE* f);

    // Used by FFmpeg log callback to count VBV underflows
    static void setGlobalInstance(PusherStats* s) { s_globalInstance = s; }
    static PusherStats* globalInstance() { return s_globalInstance; }

private:
    static PusherStats* s_globalInstance;
};
