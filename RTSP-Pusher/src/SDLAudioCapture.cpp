#include "SDLAudioCapture.h"
#include "AudioRingBuffer.h"
#include "logger/Logger.h"

SDLAudioCapture::SDLAudioCapture(AudioRingBuffer* ringBuffer, PusherStats* stats)
    : m_ringBuffer(ringBuffer), m_stats(stats) {}

SDLAudioCapture::~SDLAudioCapture() {
    close();
}

bool SDLAudioCapture::open(const PusherConfig& config, int serial) {
    m_serial = serial;

    const char* deviceName = config.audioDeviceName;
    if (config.audioDeviceIndex >= 0) {
        int count = SDL_GetNumAudioDevices(1);
        if (config.audioDeviceIndex >= count) {
            LOG_ERROR("[audio-capture] Invalid audio capture device index %d (available=%d)",
                      config.audioDeviceIndex, count);
            return false;
        }
        deviceName = SDL_GetAudioDeviceName(config.audioDeviceIndex, 1);
        if (!deviceName) {
            LOG_ERROR("[audio-capture] SDL_GetAudioDeviceName(%d) failed: %s",
                      config.audioDeviceIndex, SDL_GetError());
            return false;
        }
        LOG_INFO("[audio-capture] Selected device index %d: %s",
                 config.audioDeviceIndex, deviceName);
    }

    SDL_memset(&m_desired, 0, sizeof(m_desired));
    m_desired.freq     = config.audioSampleRate;   // 48000
    m_desired.format   = AUDIO_S16SYS;             // signed 16-bit
    m_desired.channels = config.audioChannels;      // 2
    m_desired.samples  = 1024;                      // ~21ms per callback
    m_desired.callback = sdlCaptureCallback;
    m_desired.userdata = this;

    // SDL_OpenAudioDevice with iscapture=1, allowed_changes=0 for older SDL2
    // The obtained spec will reflect actual device capabilities
    m_deviceId = SDL_OpenAudioDevice(deviceName, 1, &m_desired,
                                     &m_obtained,
                                     SDL_AUDIO_ALLOW_FREQUENCY_CHANGE
                                   | SDL_AUDIO_ALLOW_FORMAT_CHANGE
                                   | SDL_AUDIO_ALLOW_CHANNELS_CHANGE
                                   | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (m_deviceId == 0) {
        LOG_ERROR("[audio-capture] SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }

    LOG_INFO("[audio-capture] Device opened: %s  freq=%d  format=0x%x  channels=%d  samples=%d",
             deviceName ? deviceName : "(default)",
             m_obtained.freq, m_obtained.format, m_obtained.channels, m_obtained.samples);
    return true;
}

void SDLAudioCapture::start() {
    if (m_deviceId != 0) {
        m_paused = false;
        SDL_PauseAudioDevice(m_deviceId, 0);
        LOG_INFO("[audio-capture] Capture started");
    }
}

void SDLAudioCapture::stop() {
    if (m_deviceId != 0 && !m_paused) {
        SDL_PauseAudioDevice(m_deviceId, 1);
        m_paused = true;
        LOG_INFO("[audio-capture] Capture stopped");
    }
}

void SDLAudioCapture::close() {
    stop();
    if (m_deviceId != 0) {
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
        LOG_INFO("[audio-capture] Device closed");
    }
}

void SDLAudioCapture::setSerial(int serial) {
    m_serial = serial;
    if (m_ringBuffer) m_ringBuffer->setSerial(serial);
}

void SDLCALL SDLAudioCapture::sdlCaptureCallback(void* userdata, Uint8* stream, int len) {
    auto* self = static_cast<SDLAudioCapture*>(userdata);
    if (!self || !self->m_ringBuffer) return;
    if (len <= 0) return;

    int written = self->m_ringBuffer->write(stream, len);
    self->m_stats->audioBytesCaptured += written;

    if (written < len) {
        // Buffer overflow — audio data dropped
        static int overflowCount = 0;
        if (++overflowCount <= 5 || overflowCount % 100 == 0) {
            LOG_WARN("[audio-capture] Ring buffer overflow (%d/%d bytes, count=%d)",
                     written, len, overflowCount);
        }
    }
}
