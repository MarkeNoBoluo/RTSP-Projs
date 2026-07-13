#include "SDLAudio.h"
#include "AudioRingBuffer.h"
#include "AVClock.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

#include <SDL.h>
#include <cstring>

extern "C" {
#include <libavutil/time.h>
}

SDLAudio::SDLAudio(AudioRingBuffer* ringBuffer, AVClock* clock, PlayerStats* stats,
                   int desiredSamples)
    : m_ringBuffer(ringBuffer)
    , m_clock(clock)
    , m_stats(stats)
    , m_desiredSamples(desiredSamples)
{
}

SDLAudio::~SDLAudio() {
    close();
}

bool SDLAudio::init(int sampleRate, int channels) {
    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq     = sampleRate;
    desired.format   = AUDIO_S16SYS;
    desired.channels = channels;
    desired.samples  = m_desiredSamples;
    desired.callback = sdlCallback;
    desired.userdata = this;

    SDL_AudioSpec obtained;
    m_deviceId = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained,
                                     SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (m_deviceId == 0) {
        LOG_ERROR("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }

    // Use actual obtained hardware params for clock calculations
    m_sampleRate    = obtained.freq;
    m_channels      = obtained.channels;
    m_bytesPerFrame = (SDL_AUDIO_BITSIZE(obtained.format) / 8) * obtained.channels;

    if (m_ringBuffer) {
        m_ringBuffer->setAudioParams(m_sampleRate, m_bytesPerFrame);
    }

    LOG_INFO("SDL audio opened: %dHz/%dch fmt=%d samples=%d bytesPerFrame=%d",
             obtained.freq, obtained.channels, obtained.format, obtained.samples,
             m_bytesPerFrame);
    return true;
}

void SDLAudio::start() {
    if (m_deviceId) SDL_PauseAudioDevice(m_deviceId, 0);
}

void SDLAudio::stop() {
    if (m_deviceId) SDL_PauseAudioDevice(m_deviceId, 1);
}

void SDLAudio::close() {
    if (m_deviceId) {
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
    }
}

void SDLAudio::sdlCallback(void* userdata, unsigned char* stream, int len) {
    auto* self = static_cast<SDLAudio*>(userdata);

    double audioClock = 0.0;
    int read = self->m_ringBuffer->read(stream, len, &audioClock);

    if (read < len) {
        memset(stream + read, 0, len - read);
    }

    if (read > 0) {
        {
            int64_t expectedZero = 0;
            self->m_stats->audioFirstPlayUs.compare_exchange_strong(
                expectedZero, av_gettime_relative(),
                std::memory_order_release, std::memory_order_acquire);
        }
        self->m_clock->setAudioClock(audioClock);
    }
}
