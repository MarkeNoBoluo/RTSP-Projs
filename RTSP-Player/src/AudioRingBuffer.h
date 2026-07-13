#pragma once

#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <atomic>

class PlayerStats;

class AudioRingBuffer {
public:
    struct Chunk {
        uint8_t* data = nullptr;
        int32_t  len = 0;
        double   pts = 0.0;
        int      serial = 0;
    };

    AudioRingBuffer(int bufferMs = kDefaultBufferMs);
    ~AudioRingBuffer();

    static constexpr int kDefaultBufferMs = 100;
    static constexpr int kLowLatencyBufferMs = 60;

    int maxBufferBytes() const {
        return m_sampleRate * m_bytesPerFrame * m_bufferMs / 1000;
    }

    void setStats(PlayerStats* stats) { m_stats = stats; }
    void setAudioParams(int sampleRate, int bytesPerFrame) {
        m_sampleRate = sampleRate;
        m_bytesPerFrame = bytesPerFrame;
    }

    bool write(const uint8_t* data, int len, double pts, int serial);
    int  read(uint8_t* dst, int len, double* outPts, int* outChunkOffset);
    // Returns accurate end-PTS accounting for partial chunk reads
    int  read(uint8_t* dst, int len, double* outClockPts);
    void flush();
    void abort();
    int  serial() const { return m_serial.load(); }
    void setSerial(int s) { m_serial.store(s); }

    int currentFillBytes() const;
    int readEmptyCount() const;
    int writeBlockCount() const;

    // Thread-safe snapshot: locks, reads all three counters atomically
    void snapshotRingCounters(int& outFillBytes, int& outReadEmpty, int& outWriteBlocked);

private:
    static constexpr int kMaxChunks = 32;

    int m_bufferMs;
    Chunk m_chunks[kMaxChunks];

    int m_writeIdx = 0;
    int m_readIdx  = 0;
    int m_readOffset = 0;  // bytes already consumed from current chunk
    int m_avail = 0;       // number of readable chunks
    int m_totalBytes = 0;  // total bytes across all chunks in buffer

    std::atomic<int> m_serial{0};

    std::mutex m_mutex;
    std::condition_variable m_cv;

    PlayerStats* m_stats = nullptr;
    std::atomic<bool> m_abort{false};

    int m_sampleRate    = 48000;
    int m_bytesPerFrame = 4;    // stereo s16: 2ch × 2bytes

    std::atomic<int> m_readEmptyCount{0};
    std::atomic<int> m_writeBlockCount{0};
};
