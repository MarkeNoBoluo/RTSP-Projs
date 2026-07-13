#pragma once

class AudioRingBuffer;
class AVClock;
class PlayerStats;

class SDLAudio {
public:
    SDLAudio(AudioRingBuffer* ringBuffer, AVClock* clock, PlayerStats* stats,
             int desiredSamples = 1024);
    ~SDLAudio();

    bool init(int sampleRate, int channels);
    void start();
    void stop();
    void close();

    bool isOpen() const { return m_deviceId != 0; }

private:
    static void sdlCallback(void* userdata, unsigned char* stream, int len);

    AudioRingBuffer* m_ringBuffer;
    AVClock*         m_clock;
    PlayerStats*     m_stats;

    unsigned int m_deviceId = 0;
    int m_sampleRate  = 48000;
    int m_channels    = 2;
    int m_bytesPerFrame = 4;  // default stereo s16: 2ch × 2bytes
    int m_desiredSamples = 1024;
};
