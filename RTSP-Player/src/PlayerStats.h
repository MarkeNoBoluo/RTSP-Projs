#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <mutex>
#include <fstream>

class PlayerStats {
public:
    // Frame counters
    std::atomic<int64_t> framesDecoded{0};
    std::atomic<int64_t> framesRendered{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int>     reconnectCount{0};
    std::atomic<int>     queueVideoDurationMs{0};

    // Timing (all in microseconds)
    std::atomic<int64_t> lastLatenessUs{0};
    std::atomic<int64_t> maxLatenessUs{0};
    std::atomic<int64_t> renderSkipBurst{0};
    std::atomic<int64_t> totalReconnectMs{0};
    std::atomic<int>     audioUnderruns{0};
    std::atomic<int>     audioOverruns{0};
    std::atomic<int>     videoQueuePeakMs{0};
    std::atomic<int>     audioQueuePeakMs{0};
    std::atomic<int>     videoQueuePeakPkts{0};

    // Decode timing (microseconds)
    std::atomic<int64_t> decodeSendUsMax{0};
    std::atomic<int64_t> decodeReceiveUsMax{0};
    std::atomic<int>     decodeErrorCount{0};

    // FrameQueue
    std::atomic<int>     frameQueueWriteFailures{0};
    std::atomic<int>     frameQueueOverwrites{0};
    std::atomic<int>     frameQueuePeakSlots{0};

    // Stutter / stall diagnostics
    std::atomic<int>     videoPopTimeouts{0};     // decode pop timeout bursts (consecutive)
    std::atomic<int>     catchUpDrops{0};         // frames dropped in catch-up logic
    std::atomic<int>     videoStallCount{0};      // number of stall events (long pop timeouts)
    std::atomic<int64_t> frameAudDiffMs{0};       // last frame-audio diff (ms)
    std::atomic<int64_t> clockDiffMs{0};          // last clock diff (ms)

    // Audio real counters
    std::atomic<int64_t> audioPacketsReceived{0};
    std::atomic<int64_t> audioFramesDecoded{0};
    std::atomic<int64_t> audioBytesWritten{0};

    // AudioRingBuffer
    std::atomic<int>     audioRingFillBytes{0};
    std::atomic<int>     audioRingReadEmpty{0};
    std::atomic<int>     audioRingWriteBlocked{0};

    // Pacing (all in microseconds)
    std::atomic<int64_t> paintIntervalMinUs{0};
    std::atomic<int64_t> paintIntervalMaxUs{0};
    std::atomic<int64_t> paintIntervalSumUs{0};
    std::atomic<int>     paintIntervalCount{0};
    std::atomic<int64_t> paintLatencyMinUs{0};
    std::atomic<int64_t> paintLatencyMaxUs{0};
    std::atomic<int64_t> paintLatencySumUs{0};
    std::atomic<int>     paintLatencyCount{0};

    // Monotonic frame ID
    std::atomic<uint64_t> frameId{1};

    // First-frame timestamps (us, av_gettime_relative)
    std::atomic<int64_t> videoFirstDecodeUs{0};
    std::atomic<int64_t> videoFirstRenderUs{0};
    std::atomic<int64_t> audioFirstDecodeUs{0};
    std::atomic<int64_t> audioFirstPlayUs{0};

    // Cross-thread timing
    std::atomic<int64_t> lastCommitUs{0};
    std::atomic<int64_t> reconnectStartUs{0};

    // HW decode metrics
    std::atomic<bool>    hwDecodeEnabled{false};     // HW decoder active
    std::atomic<int64_t> hwDecodedFrames{0};         // cumulative HW-decoded frames
    std::atomic<int>     hwTransferFailures{0};       // av_hwframe_transfer_data failures
    std::atomic<int64_t> hwTransferMaxUs{0};          // peak transfer time (us)

    // CSV
    bool initCsv(const std::string& path);
    void writeCsvRow();
    void closeCsv();

    // Recording helpers (called from worker threads)
    void recordPaintInterval(int64_t us);
    void recordPaintLatency(int64_t us);
    void recordQueueDepth(int videoMs, int audioMs);
    void recordSkipBurstEnd();

private:
    void csvWrite(const std::string& line);

    std::mutex  m_csvMutex;
    std::ofstream m_csvFile;
    std::string m_sessionId;
    bool m_csvHeaderWritten = false;
};
