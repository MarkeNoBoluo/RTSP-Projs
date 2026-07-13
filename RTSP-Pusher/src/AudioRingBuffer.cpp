#include "AudioRingBuffer.h"
#include <cstring>
#include <algorithm>

AudioRingBuffer::AudioRingBuffer(int capacityBytes)
    : m_capacity(capacityBytes) {
    m_buffer = new uint8_t[capacityBytes];
    std::memset(m_buffer, 0, capacityBytes);
}

AudioRingBuffer::~AudioRingBuffer() {
    delete[] m_buffer;
    m_buffer = nullptr;
}

int AudioRingBuffer::write(const uint8_t* data, int len) {
    if (len <= 0) return 0;

    int avail = m_capacity - m_available.load();
    if (avail <= 0) {
        m_overflowCount++;
        return 0; // buffer full, drop
    }
    if (len > avail) {
        m_overflowCount++;
    }

    int toWrite = std::min(len, avail);
    int w = m_writePos.load();

    int firstPart = std::min(toWrite, m_capacity - w);
    std::memcpy(m_buffer + w, data, firstPart);
    if (toWrite > firstPart) {
        std::memcpy(m_buffer, data + firstPart, toWrite - firstPart);
    }

    m_writePos.store((w + toWrite) % m_capacity);
    m_available.fetch_add(toWrite);
    return toWrite;
}

int AudioRingBuffer::read(uint8_t* buf, int maxLen, bool /*blocking*/) {
    int avail = m_available.load();
    if (avail <= 0) {
        return 0;
    }
    // maxLen > avail with avail > 0 is a normal partial-read — not an underrun

    int toRead = std::min(maxLen, avail);
    int r = m_readPos.load();

    int firstPart = std::min(toRead, m_capacity - r);
    std::memcpy(buf, m_buffer + r, firstPart);
    if (toRead > firstPart) {
        std::memcpy(buf + firstPart, m_buffer, toRead - firstPart);
    }

    m_readPos.store((r + toRead) % m_capacity);
    m_available.fetch_sub(toRead);
    return toRead;
}

int AudioRingBuffer::available() const {
    return m_available.load();
}

void AudioRingBuffer::flush() {
    m_writePos = 0;
    m_readPos = 0;
    m_available = 0;
    m_overflowCount = 0;
}

void AudioRingBuffer::setSerial(int serial) {
    m_serial = serial;
}
