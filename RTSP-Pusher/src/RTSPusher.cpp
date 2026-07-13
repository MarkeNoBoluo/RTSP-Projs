#include "RTSPusher.h"
#include "PusherStateMachine.h"
#include "PusherLifecycleManager.h"
#include "ScreenCaptureThread.h"
#include "VideoEncodeThread.h"
#include "GpuVideoEncodeThread.h"
#include "AudioEncodeThread.h"
#include "RTSPMuxThread.h"
#include "SDLAudioCapture.h"
#include "VideoRawFrameQueue.h"
#include "AudioRingBuffer.h"
#include "EncodedPacketQueue.h"
#include "PusherStats.h"
#include "HardwareEncoderDetector.h"
#include "logger/Logger.h"
#include <cstring>

extern "C" {
#include <libavutil/time.h>
}

static bool useDdagrabPipeline(const PusherConfig& config) {
#if defined(_WIN64)
    return std::strcmp(config.captureMethod, "ddagrab") == 0;
#else
    (void)config;
    return false;
#endif
}

RTSPusher::RTSPusher() {
    m_stateMachine = new PusherStateMachine();
    m_lifecycle = new PusherLifecycleManager(this);
    m_stats = new PusherStats();
}

RTSPusher::~RTSPusher() {
    close();
    delete m_stats;
    m_stats = nullptr;
    delete m_lifecycle;
    m_lifecycle = nullptr;
    delete m_stateMachine;
    m_stateMachine = nullptr;
}

bool RTSPusher::open(const PusherConfig& config) {
    m_config = config;

    if (!m_stateMachine->transition(PusherState::Stopped, PusherState::Opening)) {
        LOG_WARN("open() called but state is not Stopped");
        return false;
    }

    LOG_INFO("RTSPusher::open() - url=%s  capture=%dx%d@%d  output=%dx%d  audio=%s",
             config.rtspUrl, config.captureWidth, config.captureHeight,
             config.captureFps, config.outputWidth, config.outputHeight,
             config.enableAudio ? "yes" : "no");

    if (m_stateCallback) m_stateCallback(PusherState::Opening);

    if (!startPipeline()) {
        LOG_ERROR("RTSPusher::open() failed to start pipeline");
        stopPipeline();
        m_stateMachine->forceState(PusherState::Error);
        if (m_stateCallback) m_stateCallback(PusherState::Error);
        return false;
    }

    m_stateMachine->transition(PusherState::Opening, PusherState::Streaming);

    if (m_stateCallback) m_stateCallback(PusherState::Streaming);
    LOG_INFO("RTSPusher::open() success");
    return true;
}

void RTSPusher::close() {
    if (!m_stateMachine->transition(PusherState::Streaming, PusherState::Closing) &&
        !m_stateMachine->transition(PusherState::Error, PusherState::Closing) &&
        !m_stateMachine->transition(PusherState::Recovering, PusherState::Closing)) {
        return;
    }

    LOG_INFO("RTSPusher::close()");
    if (m_stateCallback) m_stateCallback(PusherState::Closing);

    stopPipeline();

    m_stateMachine->transition(PusherState::Closing, PusherState::Stopped);
    if (m_stateCallback) m_stateCallback(PusherState::Stopped);
    LOG_INFO("RTSPusher::close() done");
}

PusherState RTSPusher::state() const {
    return m_stateMachine->state();
}

void RTSPusher::setStateCallback(StateCallback cb) {
    m_stateCallback = std::move(cb);
}

void RTSPusher::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}

bool RTSPusher::startPipeline() {
    int serial = m_lifecycle->nextSerial();
    m_serial = serial;
    m_stats->reset();
    m_stats->pipelineStartUs = av_gettime_relative();

    // Queues
    m_encodedQueue = new EncodedPacketQueue(60);

    AVCodecContext* videoCtx = nullptr;
    const bool gpuPipeline = useDdagrabPipeline(m_config);

    if (gpuPipeline) {
        const char* resolved = resolveEncoderName(m_config.hwEncoder);
        GpuBackend backend = GpuBackend::QSV;
        if (resolved && std::strstr(resolved, "nvenc")) {
            backend = GpuBackend::NVENC;
        }
        if (backend == GpuBackend::NVENC) {
            LOG_INFO("[pipeline] Using x64 GPU video path: ddagrab -> d3d11 frames -> h264_nvenc");
        } else {
            LOG_INFO("[pipeline] Using x64 GPU video path: ddagrab -> hwmap(qsv) -> scale_qsv -> h264_qsv");
        }
        m_gpuEncodeThread = new GpuVideoEncodeThread(&m_config, m_encodedQueue, m_stats, backend);
        m_gpuEncodeThread->setSerial(serial);
        m_gpuEncodeThread->setErrorCallback([this](const char* msg) {
            if (m_errorCallback) m_errorCallback(msg);
            m_lifecycle->scheduleReconnect(msg);
        });
        if (!m_gpuEncodeThread->start(serial)) {
            LOG_ERROR("Failed to start GPU video encode thread");
            return false;
        }
        videoCtx = m_gpuEncodeThread->codecContext();
    } else {
        m_rawFrameQueue = new VideoRawFrameQueue(4);
        m_encodeThread = new VideoEncodeThread(&m_config, m_rawFrameQueue, m_encodedQueue, m_stats);
        m_encodeThread->setSerial(serial);
        m_encodeThread->setErrorCallback([this](const char* msg) {
            if (m_errorCallback) m_errorCallback(msg);
            m_lifecycle->scheduleReconnect(msg);
        });
        if (!m_encodeThread->start(serial)) {
            LOG_ERROR("Failed to start video encode thread");
            return false;
        }
        videoCtx = m_encodeThread->codecContext();
    }

    // Audio (Phase 5)
    AVCodecContext* audioCtx = nullptr;
    if (m_config.enableAudio) {
        m_audioRingBuffer = new AudioRingBuffer(65536);  // ~682ms @ 48kHz stereo S16
        m_audioRingBuffer->setSerial(serial);

        m_audioEncodeThread = new AudioEncodeThread(&m_config, m_audioRingBuffer,
                                                     m_encodedQueue, m_stats);
        m_audioEncodeThread->setSerial(serial);
        m_audioEncodeThread->setErrorCallback([this](const char* msg) {
            if (m_errorCallback) m_errorCallback(msg);
        });

        m_audioCapture = new SDLAudioCapture(m_audioRingBuffer, m_stats);
        m_audioCapture->setSerial(serial);

        if (!m_audioCapture->open(m_config, serial)) {
            if (m_config.requireAudio) {
                LOG_ERROR("Failed to open required audio capture device");
                delete m_audioCapture;
                m_audioCapture = nullptr;
                delete m_audioEncodeThread;
                m_audioEncodeThread = nullptr;
                delete m_audioRingBuffer;
                m_audioRingBuffer = nullptr;
                return false;
            }
            LOG_WARN("Failed to open audio capture device, continuing without audio");
            delete m_audioCapture;
            m_audioCapture = nullptr;
            delete m_audioEncodeThread;
            m_audioEncodeThread = nullptr;
            delete m_audioRingBuffer;
            m_audioRingBuffer = nullptr;
        } else {
            if (!m_audioEncodeThread->start(m_audioCapture->obtainedSpec(), serial)) {
                if (m_config.requireAudio) {
                    LOG_ERROR("Failed to start required audio encode thread");
                    m_audioCapture->close();
                    delete m_audioCapture;
                    m_audioCapture = nullptr;
                    delete m_audioEncodeThread;
                    m_audioEncodeThread = nullptr;
                    delete m_audioRingBuffer;
                    m_audioRingBuffer = nullptr;
                    return false;
                }
                LOG_WARN("Failed to start audio encode thread, continuing without audio");
                m_audioCapture->close();
                delete m_audioCapture;
                m_audioCapture = nullptr;
                delete m_audioEncodeThread;
                m_audioEncodeThread = nullptr;
                delete m_audioRingBuffer;
                m_audioRingBuffer = nullptr;
            } else {
                audioCtx = m_audioEncodeThread->codecContext();
            }
        }
    }

    // RTSP mux
    m_muxThread = new RTSPMuxThread(&m_config, m_encodedQueue, m_stats);
    m_muxThread->setSerial(serial);
    m_muxThread->setErrorCallback([this](const char* msg) {
        if (m_errorCallback) m_errorCallback(msg);
        m_lifecycle->scheduleReconnect(msg);
    });

    if (!m_muxThread->open(videoCtx, audioCtx)) {
        LOG_ERROR("Failed to open RTSP mux output");
        return false;
    }

    if (!m_muxThread->start(serial)) {
        LOG_ERROR("Failed to start RTSP mux thread");
        return false;
    }

    // Start audio capture (if available)
    if (m_audioCapture) {
        m_audioCapture->start();
    }

    if (!gpuPipeline) {
        // Screen capture
        m_captureThread = new ScreenCaptureThread(&m_config, m_rawFrameQueue, m_stats);
        m_captureThread->setSerial(serial);
        m_captureThread->setErrorCallback([this](const char* msg) {
            if (m_errorCallback) m_errorCallback(msg);
        });

        if (!m_captureThread->start(serial)) {
            LOG_ERROR("Failed to start screen capture thread");
            return false;
        }
    }

    LOG_INFO("Pipeline started, serial=%d", serial);
    return true;
}

void RTSPusher::stopPipeline() {
    LOG_INFO("[pipeline] Stopping pipeline (serial=%d)...", m_serial.load());

    // Step 1: Stop capture (first, so no new frames enter the system)
    if (m_captureThread) {
        LOG_INFO("[pipeline]   [1/5] Stopping screen capture...");
        m_captureThread->stop();
        m_captureThread->join();
        delete m_captureThread;
        m_captureThread = nullptr;
        LOG_INFO("[pipeline]   [1/5] Screen capture stopped");
    }

    // Step 2: Stop audio (capture → encode → ring buffer)
    if (m_audioCapture) {
        LOG_INFO("[pipeline]   [2/5] Stopping audio capture...");
        m_audioCapture->stop();
        m_audioCapture->close();
        delete m_audioCapture;
        m_audioCapture = nullptr;
        LOG_INFO("[pipeline]   [2/5] Audio capture stopped");
    }

    if (m_audioEncodeThread) {
        LOG_INFO("[pipeline]   [2/5] Stopping audio encode thread...");
        m_audioEncodeThread->stop();
        m_audioEncodeThread->join();
        delete m_audioEncodeThread;
        m_audioEncodeThread = nullptr;
        LOG_INFO("[pipeline]   [2/5] Audio encode thread stopped");
    }

    if (m_audioRingBuffer) {
        m_audioRingBuffer->flush();
        delete m_audioRingBuffer;
        m_audioRingBuffer = nullptr;
    }

    // Step 3: Stop video encoder (after capture, before mux drain)
    if (m_encodeThread) {
        LOG_INFO("[pipeline]   [3/5] Stopping video encode thread...");
        m_encodeThread->stop();
        m_encodeThread->join();
        delete m_encodeThread;
        m_encodeThread = nullptr;
        LOG_INFO("[pipeline]   [3/5] Video encode thread stopped");
    }

    if (m_gpuEncodeThread) {
        LOG_INFO("[pipeline]   [3/5] Stopping GPU video encode thread...");
        m_gpuEncodeThread->stop();
        m_gpuEncodeThread->join();
        delete m_gpuEncodeThread;
        m_gpuEncodeThread = nullptr;
        LOG_INFO("[pipeline]   [3/5] GPU video encode thread stopped");
    }

    // Step 4: Stop mux (last writer — write trailer before tearing down)
    if (m_muxThread) {
        LOG_INFO("[pipeline]   [4/5] Stopping RTSP mux thread...");
        m_muxThread->stop();
        m_muxThread->join();
        m_muxThread->close();  // writes RTSP trailer
        delete m_muxThread;
        m_muxThread = nullptr;
        LOG_INFO("[pipeline]   [4/5] RTSP mux thread stopped");
    }

    // Step 5: Flush and destroy queues
    if (m_rawFrameQueue) {
        m_rawFrameQueue->flush();
        delete m_rawFrameQueue;
        m_rawFrameQueue = nullptr;
    }

    if (m_encodedQueue) {
        m_encodedQueue->flush();
        delete m_encodedQueue;
        m_encodedQueue = nullptr;
    }

    LOG_INFO("[pipeline]   [5/5] Queues flushed — all resources released");
    LOG_INFO("Pipeline stopped");
}
