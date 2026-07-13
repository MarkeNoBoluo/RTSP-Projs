#pragma once

#include <atomic>
#include <cstdint>

// Lock-free single-producer, single-consumer ring buffer for PCM audio.
// Producer: SDL audio callback. Consumer: audio encode thread.

class AudioRingBuffer {
public:
    AudioRingBuffer(int capacityBytes = 65536);
    ~AudioRingBuffer();

    // Write PCM data (called from SDL callback).
    // Returns number of bytes actually written.
    int write(const uint8_t* data, int len);

    // Read PCM data (called from encode thread).
    // Returns number of bytes actually read.
    int read(uint8_t* buf, int maxLen, bool blocking = false);

    int  available() const;   // readable bytes
    int  overflowCount() const { return m_overflowCount.load(); }
    void flush();
    void setSerial(int serial);
    int  serial() const { return m_serial.load(); }

private:
    uint8_t* m_buffer = nullptr;
    int m_capacity;
    std::atomic<int> m_writePos{0};
    std::atomic<int> m_readPos{0};
    std::atomic<int> m_serial{0};
    std::atomic<int> m_available{0};
    std::atomic<int> m_overflowCount{0};
};
