#pragma once

#include "PusherConfig.h"
#include "PusherStats.h"
#include <SDL.h>
#include <atomic>

class AudioRingBuffer;

class SDLAudioCapture {
public:
    SDLAudioCapture(AudioRingBuffer* ringBuffer, PusherStats* stats);
    ~SDLAudioCapture();

    bool open(const PusherConfig& config, int serial);
    void start();
    void stop();
    void close();

    void setSerial(int serial);
    const SDL_AudioSpec& obtainedSpec() const { return m_obtained; }
    bool isOpen() const { return m_deviceId != 0; }

private:
    static void SDLCALL sdlCaptureCallback(void* userdata, Uint8* stream, int len);

    AudioRingBuffer* m_ringBuffer = nullptr;
    PusherStats*     m_stats = nullptr;
    SDL_AudioDeviceID m_deviceId = 0;
    SDL_AudioSpec     m_obtained{};
    SDL_AudioSpec     m_desired{};
    std::atomic<int>  m_serial{0};
    std::atomic<bool> m_paused{false};
};
