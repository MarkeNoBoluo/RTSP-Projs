#include "AudioRingBuffer.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

#include <algorithm>
#include <cstring>

AudioRingBuffer::AudioRingBuffer(int bufferMs)
    : m_bufferMs(bufferMs)
{
}

AudioRingBuffer::~AudioRingBuffer() {
    for (int i = 0; i < kMaxChunks; i++) {
        delete[] m_chunks[i].data;
    }
}

bool AudioRingBuffer::write(const uint8_t* data, int len, double pts, int serial) {
    if (len <= 0) return true;
    if (serial != m_serial.load(std::memory_order_acquire)) return false;

    std::unique_lock<std::mutex> lock(m_mutex);

    // Discard oldest chunks until there's room (by bytes or by chunk count)
    int maxBytes = maxBufferBytes();
    while (m_avail > 0 && (m_avail >= kMaxChunks || m_totalBytes + len > maxBytes)) {
        Chunk& oldest = m_chunks[m_readIdx];
        m_totalBytes -= oldest.len;
        delete[] oldest.data;
        oldest.data = nullptr;
        oldest.len  = 0;
        m_readIdx  = (m_readIdx + 1) % kMaxChunks;
        if (m_readOffset > 0) m_readOffset = 0;
        m_avail--;
    }

    if (m_abort) return false;

    Chunk& chunk = m_chunks[m_writeIdx];
    delete[] chunk.data;
    chunk.data = new uint8_t[len];
    memcpy(chunk.data, data, len);
    chunk.len = len;
    chunk.pts = pts;
    chunk.serial = serial;

    m_writeIdx = (m_writeIdx + 1) % kMaxChunks;
    m_avail++;
    m_totalBytes += len;

    lock.unlock();
    m_cv.notify_one();
    return true;
}

int AudioRingBuffer::read(uint8_t* dst, int len, double* outPts, int* outChunkOffset) {
    std::lock_guard<std::mutex> lock(m_mutex);

    int filled = 0;
    bool firstChunk = true;

    while (filled < len) {
        if (m_avail <= 0) {
            m_readEmptyCount.fetch_add(1, std::memory_order_relaxed);
            if (m_stats) m_stats->audioUnderruns++;
            break;
        }

        Chunk& chunk = m_chunks[m_readIdx];
        int remaining = chunk.len - m_readOffset;
        int toCopy = std::min(len - filled, remaining);

        memcpy(dst + filled, chunk.data + m_readOffset, toCopy);
        m_readOffset += toCopy;
        filled += toCopy;

        if (firstChunk && outPts) {
            *outPts = chunk.pts;
            firstChunk = false;
        }

        if (m_readOffset >= chunk.len) {
            m_totalBytes -= chunk.len;
            delete[] chunk.data;
            chunk.data = nullptr;
            chunk.len  = 0;
            m_readIdx  = (m_readIdx + 1) % kMaxChunks;
            m_readOffset = 0;
            m_avail--;
        }
    }

    if (outChunkOffset) *outChunkOffset = filled;
    m_cv.notify_one();
    return filled;
}

int AudioRingBuffer::read(uint8_t* dst, int len, double* outClockPts) {
    std::lock_guard<std::mutex> lock(m_mutex);

    int filled = 0;
    double actualStartPts = 0.0;
    bool haveStartPts = false;
    double bytesPerSec = (double)m_sampleRate * m_bytesPerFrame;

    while (filled < len) {
        if (m_avail <= 0) {
            m_readEmptyCount.fetch_add(1, std::memory_order_relaxed);
            if (m_stats) m_stats->audioUnderruns++;
            break;
        }

        Chunk& chunk = m_chunks[m_readIdx];
        int remaining = chunk.len - m_readOffset;
        int toCopy = std::min(len - filled, remaining);

        if (!haveStartPts) {
            // Start PTS = chunk.pts + offset within chunk (in seconds)
            actualStartPts = chunk.pts + (double)m_readOffset / bytesPerSec;
            haveStartPts = true;
        }

        memcpy(dst + filled, chunk.data + m_readOffset, toCopy);
        m_readOffset += toCopy;
        filled += toCopy;

        if (m_readOffset >= chunk.len) {
            m_totalBytes -= chunk.len;
            delete[] chunk.data;
            chunk.data = nullptr;
            chunk.len  = 0;
            m_readIdx  = (m_readIdx + 1) % kMaxChunks;
            m_readOffset = 0;
            m_avail--;
        }
    }

    if (outClockPts && haveStartPts) {
        // End PTS = actual start + duration of data consumed
        *outClockPts = actualStartPts + (double)filled / bytesPerSec;
    }

    m_cv.notify_one();
    return filled;
}

void AudioRingBuffer::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < kMaxChunks; i++) {
        delete[] m_chunks[i].data;
        m_chunks[i].data = nullptr;
        m_chunks[i].len = 0;
    }
    m_writeIdx   = 0;
    m_readIdx    = 0;
    m_readOffset = 0;
    m_avail      = 0;
    m_totalBytes = 0;
    m_writeBlockCount.store(0, std::memory_order_relaxed);
    m_readEmptyCount.store(0, std::memory_order_relaxed);
    m_serial.fetch_add(1);
    m_abort = false;
}

void AudioRingBuffer::abort() {
    m_abort = true;
    m_cv.notify_all();
}

int AudioRingBuffer::currentFillBytes() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_mutex));
    int total = 0;
    for (int i = 0; i < m_avail; i++) {
        int idx = (m_readIdx + i) % kMaxChunks;
        total += m_chunks[idx].len;
    }
    return total;
}

int AudioRingBuffer::readEmptyCount() const {
    return m_readEmptyCount.load(std::memory_order_relaxed);
}

int AudioRingBuffer::writeBlockCount() const {
    return m_writeBlockCount.load(std::memory_order_relaxed);
}

void AudioRingBuffer::snapshotRingCounters(int& outFillBytes, int& outReadEmpty, int& outWriteBlocked) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outFillBytes = 0;
    for (int i = 0; i < m_avail; i++) {
        int idx = (m_readIdx + i) % kMaxChunks;
        outFillBytes += m_chunks[idx].len;
    }
    outReadEmpty    = m_readEmptyCount.load(std::memory_order_relaxed);
    outWriteBlocked = m_writeBlockCount.load(std::memory_order_relaxed);
}
