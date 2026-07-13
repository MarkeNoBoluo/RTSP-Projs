#include "StreamLifecycleManager.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "PlayerStats.h"
#include "DemuxThread.h"
#include "VideoDecodeThread.h"
#include "AudioWorker.h"
#include "AudioRingBuffer.h"
#include "SDLAudio.h"
#include "logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/time.h>
}

StreamLifecycleManager::StreamLifecycleManager(
    PlayerStateMachine* sm, PlayerStats* stats,
    PacketQueue* videoQ, PacketQueue* audioQ,
    VideoFrameQueue* frameQ, AVClock* clock)
    : m_stateMachine(sm)
    , m_stats(stats)
    , m_videoQueue(videoQ)
    , m_audioQueue(audioQ)
    , m_frameQueue(frameQ)
    , m_clock(clock)
    ,m_audioEnabled(true)
{
}

StreamLifecycleManager::~StreamLifecycleManager() {
    close();
    m_stats->closeCsv();
}

PlayerState StreamLifecycleManager::state() const {
    return m_stateMachine->state();
}

bool StreamLifecycleManager::open(const char* url) {
    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    m_stateMachine->forceState(PlayerState::Stopped);
    shutdownPipeline();

    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    m_generation++;
    m_url = url;
    m_backoffCount = 0;

    if (!initDemux(url)) {
        m_stateMachine->forceState(PlayerState::Error);
        if (m_onState) m_onState(PlayerState::Error);
        if (m_onError) m_onError("Failed to open RTSP stream");
        return false;
    }

    if (!initDecoders()) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        m_stateMachine->forceState(PlayerState::Error);
        if (m_onState) m_onState(PlayerState::Error);
        if (m_onError) m_onError("Failed to open decoder");
        return false;
    }

    startThreads();
    return true;
}

void StreamLifecycleManager::close() {
    if (m_stateMachine->state() == PlayerState::Stopped) return;

    m_generation++;

    m_stateMachine->transition(m_stateMachine->state(), PlayerState::Closing);
    if (m_onState) m_onState(PlayerState::Closing);

    shutdownPipeline();
    m_stateMachine->forceState(PlayerState::Stopped);
    if (m_onState) m_onState(PlayerState::Stopped);
}

int StreamLifecycleManager::calcBackoffMs() {
    int delay = 1000 << std::min(m_backoffCount, 3);
    m_backoffCount++;
    return delay;
}

bool StreamLifecycleManager::initDemux(const char* url) {
    LOG_INFO("Opening stream: %s", url);

    delete m_demuxThread;
    m_demuxThread = new DemuxThread(m_stateMachine, m_stats,
                                    m_videoQueue, m_audioQueue);
    m_demuxThread->prepareForOpen();
    m_demuxThread->setStreamErrorCallback([this]() {
        if (!m_stateMachine->transition(PlayerState::Playing, PlayerState::Recovering)) {
            return;
        }
        if (m_onState) m_onState(PlayerState::Recovering);
        m_stats->reconnectStartUs.store(av_gettime_relative());
        LOG_INFO("Stream error detected, shutting down for reconnect");
        shutdownPipeline();
        scheduleReconnect();
    });
    m_demuxThread->setEndOfStreamCallback([this]() {
        LOG_INFO("End of stream detected, requesting application exit");
        if (m_onEndOfStream) m_onEndOfStream();
    });

    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        LOG_ERROR("Failed to allocate format context");
        return false;
    }

    AVIOInterruptCB intrCb = { DemuxThread::interruptCallback, m_demuxThread };
    m_fmtCtx->interrupt_callback = intrCb;
    m_fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    m_fmtCtx->max_delay = m_lowLatency ? 0 : 100000;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", m_transport.c_str(), 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "probesize", m_lowLatency ? "2048" : "32000", 0);
    av_dict_set(&opts, "analyzeduration", "0", 0);
    av_dict_set(&opts, "max_delay", m_lowLatency ? "0" : "100000", 0);

    int ret = avformat_open_input(&m_fmtCtx, url, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_open_input failed: %s", errbuf);
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    m_fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    m_fmtCtx->max_delay = m_lowLatency ? 0 : 100000;
    m_fmtCtx->max_analyze_duration = 0;

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_find_stream_info failed: %s", errbuf);
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    int videoIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // Fallback: if av_find_best_stream fails, manually search for audio stream
    if (audioIdx < 0) {
        for (unsigned i = 0; i < m_fmtCtx->nb_streams; i++) {
            if (m_fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioIdx = (int)i;
                LOG_INFO("Audio stream fallback: found stream %d (codec=%d)",
                         audioIdx, m_fmtCtx->streams[i]->codecpar->codec_id);
                break;
            }
        }
    }

    if (videoIdx >= 0) {
        m_videoStream   = m_fmtCtx->streams[videoIdx];
        m_videoCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_videoCodecPar, m_videoStream->codecpar);
        int64_t videoQueueCapacityMs = m_lowLatency
            ? m_lowLatencyVideoQueueCapacityMs
            : m_videoQueueCapacityMs;
        m_videoQueue->init(m_videoStream->time_base, static_cast<int>(videoQueueCapacityMs), "video", false, true);
        LOG_INFO("Video stream: index=%d, codec=%d, %dx%d",
                 videoIdx, m_videoCodecPar->codec_id,
                 m_videoCodecPar->width, m_videoCodecPar->height);
    }

    if (audioIdx >= 0 && m_audioEnabled) {
        m_audioStream   = m_fmtCtx->streams[audioIdx];
        m_audioCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_audioCodecPar, m_audioStream->codecpar);
        int64_t audioQueueCapacityMs = m_lowLatency ? m_lowLatencyAudioQueueCapacityMs : 200;
        m_audioQueue->init(m_audioStream->time_base, static_cast<int>(audioQueueCapacityMs), "audio", m_lowLatency);
#if LIBAVUTIL_VERSION_MAJOR >= 57
        int audioChannels = m_audioCodecPar->ch_layout.nb_channels;
#else
        int audioChannels = m_audioCodecPar->channels;
#endif
        LOG_INFO("Audio stream: index=%d, codec=%d, %dHz/%dch",
                 audioIdx, m_audioCodecPar->codec_id,
                 m_audioCodecPar->sample_rate, audioChannels);
    }

    if (!m_videoStream) {
        LOG_ERROR("No video stream found");
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    m_demuxThread->setContext(m_fmtCtx, m_videoStream, m_audioStream);

    LOG_INFO("Demux initialization complete");
    return true;
}

bool StreamLifecycleManager::initDecoders() {
    if (m_videoCodecPar) {
        const AVCodec* codec = avcodec_find_decoder(m_videoCodecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Video decoder not found");
            return false;
        }
        m_videoCodecCtx = avcodec_alloc_context3(codec);
        if (!m_videoCodecCtx) return false;
        avcodec_parameters_to_context(m_videoCodecCtx, m_videoCodecPar);

        // DXVA2 hardware decode setup
        bool enableDxva2 = (m_hwAccelMode != HwAccelMode::None);
#if defined(_WIN32) && !defined(_WIN64)
        if (m_hwAccelMode == HwAccelMode::Auto) {
            enableDxva2 = false;
            LOG_WARN("DXVA2 auto disabled for 32-bit process; using software decode. Use --hwaccel dxva2 to force hardware decode.");
        }
#endif
        if (enableDxva2) {
            bool hwFound = false;
            for (int i = 0;; i++) {
                const AVCodecHWConfig* hwCfg = avcodec_get_hw_config(codec, i);
                if (!hwCfg) break;
                if (hwCfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                    && hwCfg->device_type == AV_HWDEVICE_TYPE_DXVA2) {
                    hwFound = true;
                    break;
                }
            }
            if (hwFound) {
                int hwRet = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_DXVA2,
                                                    nullptr, nullptr, 0);
                if (hwRet >= 0) {
                    m_videoCodecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                    m_stats->hwDecodeEnabled.store(true);
                    LOG_INFO("DXVA2 hardware decoder enabled");
                } else {
                    if (m_hwAccelMode == HwAccelMode::Dxva2) {
                        LOG_ERROR("DXVA2 hardware device creation failed (forced mode)");
                        return false;
                    }
                    LOG_WARN("DXVA2 hardware device creation failed, falling back to software decode");
                }
            } else {
                if (m_hwAccelMode == HwAccelMode::Dxva2) {
                    LOG_ERROR("DXVA2 hardware config not found for this codec (forced mode)");
                    return false;
                }
                LOG_INFO("No DXVA2 hardware config for this codec, using software decode");
            }
        }

        if (m_lowLatency) {
            m_videoCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            m_videoCodecCtx->thread_count = 1;
            m_videoCodecCtx->thread_type = FF_THREAD_SLICE;
        } else {
            m_videoCodecCtx->thread_count = 0;
        }
        if (avcodec_open2(m_videoCodecCtx, codec, nullptr) < 0) return false;
        LOG_INFO("Video decoder opened: %dx%d", m_videoCodecCtx->width, m_videoCodecCtx->height);
    }

    if (m_audioCodecPar && m_audioEnabled) {
        const AVCodec* codec = avcodec_find_decoder(m_audioCodecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Audio decoder not found");
            return false;
        }
        m_audioCodecCtx = avcodec_alloc_context3(codec);
        if (!m_audioCodecCtx) return false;
        avcodec_parameters_to_context(m_audioCodecCtx, m_audioCodecPar);
        m_audioCodecCtx->thread_count = 1;
        if (m_lowLatency) {
            m_audioCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        }
        if (avcodec_open2(m_audioCodecCtx, codec, nullptr) < 0) return false;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        int decChannels = m_audioCodecCtx->ch_layout.nb_channels;
#else
        int decChannels = m_audioCodecCtx->channels;
#endif
        LOG_INFO("Audio decoder opened: %dHz/%dch",
                 m_audioCodecCtx->sample_rate, decChannels);
    }

    return true;
}

void StreamLifecycleManager::startThreads() {
    delete m_decodeThread;
    m_decodeThread = nullptr;

    AVRational videoTimeBase = m_videoStream ? m_videoStream->time_base : AVRational{1, 90000};

    if (m_videoCodecCtx) {
        m_decodeThread = new VideoDecodeThread(m_videoCodecCtx, videoTimeBase,
                                                m_videoQueue, m_frameQueue, m_stats);
    }

    // Audio pipeline
    if (m_audioCodecCtx && m_audioEnabled) {
        delete m_audioWorker;
        m_audioWorker = nullptr;
        delete m_audioRingBuffer;
        m_audioRingBuffer = nullptr;
        delete m_sdlAudio;
        m_sdlAudio = nullptr;

        AVRational audioTimeBase = m_audioStream ? m_audioStream->time_base : AVRational{1, 90000};

        m_audioRingBuffer = new AudioRingBuffer(m_lowLatency ? static_cast<int>(m_lowLatencyAudioRingBufferMs) : 100);
        m_audioRingBuffer->setStats(m_stats);

        int desiredSamples = m_lowLatency ? 512 : 1024;
        m_sdlAudio = new SDLAudio(m_audioRingBuffer, m_clock, m_stats, desiredSamples);
        if (!m_sdlAudio->init(48000, 2)) {
            LOG_WARN("SDL audio init failed, audio disabled");
            delete m_sdlAudio;
            m_sdlAudio = nullptr;
            delete m_audioRingBuffer;
            m_audioRingBuffer = nullptr;
        } else {
            m_audioWorker = new AudioWorker(m_audioCodecCtx, audioTimeBase,
                                              m_audioQueue, m_clock,
                                              m_audioRingBuffer, m_stats);
            LOG_INFO("Audio pipeline: SDLAudio(samples=%d) + AudioRingBuffer(%dms)",
                     desiredSamples, m_lowLatency ? static_cast<int>(m_lowLatencyAudioRingBufferMs) : 100);
        }
    }

    // Set initial serial
    incrementSerial();

    // Start decoder threads first, wait for ready, then start demux
    if (m_decodeThread) m_decodeThread->start();
    if (m_audioWorker && m_audioEnabled) m_audioWorker->start();
    if (m_sdlAudio && m_audioEnabled) m_sdlAudio->start();

    // Wait for video decode thread to signal ready (timeout 2s)
    if (m_decodeThread) {
        int waited = 0;
        while (!m_decodeThread->isReady() && waited < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
        }
        if (!m_decodeThread->isReady()) {
            LOG_WARN("Video decode thread not ready after %dms, starting demux anyway", waited);
        } else {
            LOG_DEBUG("Video decode thread ready after %dms", waited);
        }
    }

    // Wait for audio worker to signal ready (timeout 2s)
    if (m_audioWorker && m_audioEnabled) {
        int waited = 0;
        while (!m_audioWorker->isReady() && waited < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
        }
        if (!m_audioWorker->isReady()) {
            LOG_WARN("Audio worker not ready after %dms, starting demux anyway", waited);
        } else {
            LOG_DEBUG("Audio worker ready after %dms", waited);
        }
    }

    // Now start demux — decoders are ready to consume
    m_demuxThread->start();

    LOG_INFO("All threads started");
}

void StreamLifecycleManager::shutdownPipeline() {
    LOG_INFO("Shutting down pipeline");

    if (m_reconnectTimerId) {
        SDL_RemoveTimer(m_reconnectTimerId);
        m_reconnectTimerId = 0;
    }

    m_videoQueue->abort();
    m_audioQueue->abort();

    if (m_sdlAudio && m_audioEnabled) {
        m_sdlAudio->stop();
    }

    if (m_audioWorker && m_audioEnabled) {
        m_audioWorker->stop();
        m_audioWorker->join();
    }

    if (m_demuxThread) {
        m_demuxThread->stop();
        m_demuxThread->join();
    }
    if (m_decodeThread) {
        m_decodeThread->stop();
        m_decodeThread->join();
    }

    if (m_sdlAudio && m_audioEnabled) {
        m_sdlAudio->close();
    }

    delete m_demuxThread;
    delete m_decodeThread;
    

    m_demuxThread     = nullptr;
    m_decodeThread    = nullptr;

    if(m_audioEnabled){
        delete m_audioWorker;
        delete m_sdlAudio;
        delete m_audioRingBuffer;
        m_audioWorker     = nullptr;
        m_sdlAudio        = nullptr;
        m_audioRingBuffer = nullptr;
    }

    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }
    m_stats->hwDecodeEnabled.store(false);

    if (m_videoCodecCtx) { avcodec_flush_buffers(m_videoCodecCtx); avcodec_free_context(&m_videoCodecCtx); }
    if (m_audioCodecCtx) { avcodec_flush_buffers(m_audioCodecCtx); avcodec_free_context(&m_audioCodecCtx); }

    if (m_videoCodecPar) { avcodec_parameters_free(&m_videoCodecPar); }
    if (m_audioCodecPar) { avcodec_parameters_free(&m_audioCodecPar); }

    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
    }

    m_fmtCtx         = nullptr;
    m_videoStream    = nullptr;
    m_audioStream    = nullptr;
    m_videoCodecPar  = nullptr;
    m_audioCodecPar  = nullptr;
    m_videoCodecCtx  = nullptr;
    m_audioCodecCtx  = nullptr;

    m_videoQueue->flush();
    m_audioQueue->flush();
    m_frameQueue->flush();
    m_clock->reset();

    LOG_INFO("Pipeline shutdown complete");
}

static Uint32 onReconnectTimer(Uint32 interval, void* param) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = EVENT_RECONNECT;
    event.user.data1 = param;
    SDL_PushEvent(&event);
    return 0; // non-repeating
}

void StreamLifecycleManager::scheduleReconnect() {
    if (m_backoffCount >= kMaxBackoffAttempts) {
        LOG_ERROR("Max reconnect attempts (%d) exhausted, giving up", kMaxBackoffAttempts);
        m_stateMachine->forceState(PlayerState::Error);
        if (m_onState) m_onState(PlayerState::Error);
        if (m_onError) m_onError("Max reconnect attempts exhausted");
        return;
    }

    m_stateMachine->transition(PlayerState::Recovering, PlayerState::Reconnecting);
    if (m_onState) m_onState(PlayerState::Reconnecting);

    int delay = calcBackoffMs();
    LOG_INFO("Reconnecting in %d ms (attempt #%d)", delay, m_backoffCount);

    m_reconnectTimerId = SDL_AddTimer(delay, onReconnectTimer, this);
}

void StreamLifecycleManager::doReconnect() {
    m_reconnectTimerId = 0;

    if (!m_stateMachine->transition(PlayerState::Reconnecting, PlayerState::Connecting)) {
        LOG_INFO("Reconnect skipped: not in Reconnecting state");
        return;
    }
    if (m_onState) m_onState(PlayerState::Connecting);

    LOG_INFO("Attempting reconnect #%d to %s", m_stats->reconnectCount.load() + 1, m_url.c_str());

    if (!initDemux(m_url.c_str())) {
        scheduleReconnect();
        return;
    }

    if (!initDecoders()) {
        shutdownPipeline();
        scheduleReconnect();
        return;
    }

    m_backoffCount = 0;

    m_stats->reconnectCount++;
    int64_t reconnectUs = m_stats->reconnectStartUs.load();
    if (reconnectUs > 0) {
        int64_t recoveryMs = (av_gettime_relative() - reconnectUs) / 1000;
        m_stats->totalReconnectMs.fetch_add(recoveryMs);
        m_stats->reconnectStartUs.store(0);
    }

    startThreads();
    LOG_INFO("Reconnect successful");
}

void StreamLifecycleManager::incrementSerial() {
    int s = m_pktSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (m_demuxThread) m_demuxThread->setSerial(s);
    if (m_decodeThread) m_decodeThread->setSerial(s);
    if (m_audioWorker && m_audioEnabled) m_audioWorker->setSerial(s);
    if (m_audioRingBuffer && m_audioEnabled) m_audioRingBuffer->setSerial(s);
}

void StreamLifecycleManager::setTransport(const char* transport) {
    if (transport && transport[0]) {
        m_transport = transport;
    }
}

void StreamLifecycleManager::setHwAccel(const char* mode) {
    if (!mode || !mode[0]) return;
    m_hwAccel = mode;
    if (std::strcmp(mode, "dxva2") == 0) {
        m_hwAccelMode = HwAccelMode::Dxva2;
    } else if (std::strcmp(mode, "none") == 0) {
        m_hwAccelMode = HwAccelMode::None;
    } else {
        m_hwAccelMode = HwAccelMode::Auto;
    }
    LOG_INFO("HWAccel mode set: %s", m_hwAccel.c_str());
}
