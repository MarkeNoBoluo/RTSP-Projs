#include "RTSPlayer.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "PlayerStats.h"
#include "StreamLifecycleManager.h"
#include "AudioRingBuffer.h"
#include "SDLRenderer.h"
#include "logger/Logger.h"

#include <algorithm>

extern "C" {
#include <libavutil/time.h>
}

RTSPlayer::RTSPlayer()
    : m_stateMachine(new PlayerStateMachine)
    , m_videoQueue(new PacketQueue)
    , m_audioQueue(new PacketQueue)
    , m_frameQueue(new VideoFrameQueue)
    , m_clock(new AVClock)
    , m_stats(new PlayerStats)
{
    // Renderer created externally via main.cpp and passed in
    m_lifecycle = new StreamLifecycleManager(m_stateMachine, m_stats,
                                              m_videoQueue, m_audioQueue,
                                              m_frameQueue, m_clock);
}

RTSPlayer::~RTSPlayer() {
    close();
    delete m_lifecycle;
    delete m_stateMachine;
    delete m_videoQueue;
    delete m_audioQueue;
    delete m_frameQueue;
    delete m_clock;
    delete m_stats;
}

bool RTSPlayer::open(const char* url) {
    return m_lifecycle->open(url);
}

void RTSPlayer::close() {
    m_lifecycle->close();
}

void RTSPlayer::setRenderer(IRenderer* renderer) {
    m_renderer = renderer;
}

IRenderer* RTSPlayer::renderer() const {
    return m_renderer;
}

PlayerStats* RTSPlayer::stats() const {
    return m_lifecycle->stats();
}

PlayerState RTSPlayer::state() const {
    return m_lifecycle->state();
}

void RTSPlayer::setStateCallback(StateCallback cb) {
    m_lifecycle->setStateCallback(std::move(cb));
}

void RTSPlayer::setErrorCallback(ErrorCallback cb) {
    m_lifecycle->setErrorCallback(std::move(cb));
}

void RTSPlayer::setEndOfStreamCallback(EndOfStreamCallback cb) {
    m_lifecycle->setEndOfStreamCallback(std::move(cb));
}

void RTSPlayer::setTransport(const char* transport) {
    m_lifecycle->setTransport(transport);
}

void RTSPlayer::setAudioEnabled(bool v) {
    m_lifecycle->setAudioEnabled(v);
}

void RTSPlayer::setSetptsZero(bool v) {
    m_setptsZero = v;
    m_lowLatency = v;
    m_lifecycle->setSetptsZero(v);
}

void RTSPlayer::setHwAccel(const char* mode) {
    m_lifecycle->setHwAccel(mode);
}

int RTSPlayer::pktSerial() const {
    return m_lifecycle->pktSerial();
}

void RTSPlayer::videoRefresh() {
    if (m_inVideoRefresh) return;
    m_inVideoRefresh = true;
    struct _ { bool& flag; ~_() { flag = false; } } _guard{m_inVideoRefresh};

    constexpr double DEFAULT_FRAME_DURATION   = 0.033;
    constexpr double SYNC_THRESHOLD           = 0.040;
    constexpr double maxAudioLagForQueue_NORMAL = 0.250;
    constexpr double maxAudioLagForQueue_LOWLAT = 0.080;
    double maxAudioLagForQueue = m_lowLatency ? maxAudioLagForQueue_LOWLAT : maxAudioLagForQueue_NORMAL;
    constexpr int64_t DROP_THRESHOLD_US       = 50000;
    constexpr int64_t MIN_DROP_INTERVAL_US    = 33000;

    // 1. Drain old-generation frames + detect serial change
    bool serialChanged = false;
    while (m_frameQueue->hasNewFrame()) {
        int globalSerial = pktSerial();
        int frameSerial = m_frameQueue->peekDisplaySerial();
        if (frameSerial >= 0 && frameSerial != globalSerial) {
            m_frameQueue->discardAndAdvance();
            serialChanged = true;
            continue;
        }
        break;
    }

    if (serialChanged || m_frameTimerSerial != pktSerial()) {
        m_frameTimer = 0.0;
        m_frameTimerSerial = pktSerial();
    }

    if (!m_frameQueue->hasNewFrame()) return;

    if (m_setptsZero && !m_clock->hasAudio()) {
        int dropped = 0;
        while (m_frameQueue->count() > 1) {
            m_frameQueue->discardAndAdvance();
            m_stats->framesDropped++;
            m_stats->catchUpDrops++;
            dropped++;
        }

        if (dropped > 0) {
            m_frameTimer = 0.0;
            m_lastDropUs = av_gettime_relative();
            static int64_t s_lastSetptsDropLogUs = 0;
            if (m_lastDropUs - s_lastSetptsDropLogUs > 1000000) {
                LOG_INFO("Setpts-zero drop: dropped=%d queue=%d",
                         dropped, m_frameQueue->count());
                s_lastSetptsDropLogUs = m_lastDropUs;
            }
        }
    }

    int64_t pts = m_frameQueue->peekDisplayPts();
    if (pts < 0) return;

    // 2. First frame: render immediately
    if (!m_clock->isReady()) {
        goto render;
    }

    double targetTime = 0.0;

    // 3. Time calculations
    {
        int64_t nowUs = av_gettime_relative();
        double nowSec = nowUs / (double)AV_TIME_BASE;
        double framePtsSec = pts / (double)AV_TIME_BASE;

        // 4. PTS gap between frames (diagnostic only, not used for pacing)
        double ptsGap = DEFAULT_FRAME_DURATION;
        if (m_frameLastPts > 0.0) {
            double d = framePtsSec - m_frameLastPts;
            if (d > 0.0) ptsGap = d;
        }

        // 5. compute_target_delay (pacing always based on DEFAULT_FRAME_DURATION)
        double targetDelay = DEFAULT_FRAME_DURATION;
        bool bHasAudio = m_clock->hasAudio();

        if (bHasAudio) {
            auto vc = m_clock->videoClock();
            auto ac = m_clock->audioClock();
            double videoClockNow = vc.pts + (nowUs - vc.systemTime) / (double)AV_TIME_BASE;
            double audioClockNow = ac.pts + (nowUs - ac.systemTime) / (double)AV_TIME_BASE;
            double avDiff = framePtsSec - audioClockNow;

            if (avDiff < -maxAudioLagForQueue) {
                if (m_frameQueue->count() > 1) {
                    // Normal catch-up: drop queued frames to skip ahead
                    int dropped = 0;
                    while (m_frameQueue->count() > 1 && avDiff < -maxAudioLagForQueue) {
                        m_frameQueue->discardAndAdvance();
                        m_stats->framesDropped++;
                        m_stats->catchUpDrops++;
                        dropped++;

                        pts = m_frameQueue->peekDisplayPts();
                        if (pts < 0) return;
                        framePtsSec = pts / (double)AV_TIME_BASE;
                        avDiff = framePtsSec - audioClockNow;
                    }

                    if (dropped > 0) {
                        m_frameTimer = nowSec;
                        m_lastDropUs = nowUs;

                        if (m_frameLastPts > 0.0) {
                            double d = framePtsSec - m_frameLastPts;
                            ptsGap = d > 0.0 ? d : DEFAULT_FRAME_DURATION;
                        }

                        static int64_t s_lastCatchUpLogUs = 0;
                        if (nowUs - s_lastCatchUpLogUs > 1000000) {
                            LOG_INFO("Catch-up drop: dropped=%d frameAudDiff=%.3f pts=%.3f aud=%.3f queue=%d",
                                     dropped, avDiff, framePtsSec, audioClockNow, m_frameQueue->count());
                            s_lastCatchUpLogUs = nowUs;
                        }
                    }
                } else if (m_frameQueue->count() == 1) {
                    // Fast catch-up: only 1 frame, can't drop — unpaced rendering
                    if (!m_fastCatchUp) {
                        m_fastCatchUp = true;
                        m_fastCatchUpFrameCount = 0;
                        LOG_INFO("Fast catch-up mode ON: frameAudDiff=%.3f queue=%d",
                                 avDiff, m_frameQueue->count());
                    }
                }
            } else if (m_fastCatchUp && avDiff >= -SYNC_THRESHOLD) {
                // Exit fast catch-up once back in sync
                LOG_INFO("Fast catch-up mode OFF: caught up in %d frames, frameAudDiff=%.3f",
                         m_fastCatchUpFrameCount, avDiff);
                m_fastCatchUp = false;
                m_fastCatchUpFrameCount = 0;
            }

            if (avDiff > SYNC_THRESHOLD) {
                targetDelay = std::min(0.050, DEFAULT_FRAME_DURATION + SYNC_THRESHOLD);
            } else if (avDiff < -SYNC_THRESHOLD) {
                targetDelay = m_fastCatchUp ? 0.001 : 0.010;
            } else if (m_fastCatchUp) {
                // Still catching up but within ±40ms — keep minimal delay
                targetDelay = 0.005;
            }

            if (targetDelay < 0.010) targetDelay = m_fastCatchUp ? 0.001 : 0.010;
            if (targetDelay > 0.100) targetDelay = 0.100;

            // Write per-interval metrics for CSV
            m_stats->frameAudDiffMs.store(static_cast<int64_t>(avDiff * 1000), std::memory_order_relaxed);
            m_stats->clockDiffMs.store(static_cast<int64_t>((videoClockNow - audioClockNow) * 1000), std::memory_order_relaxed);

            static int64_t s_lastAvDiffLogUs = 0;
            int64_t nowUsForLog = av_gettime_relative();
            auto rn = m_stats->framesRendered.load();
            if (rn <= 3 || nowUsForLog - s_lastAvDiffLogUs > 1000000) {
                LOG_INFO("frameAudDiff=%.3f clockDiff=%.3f vid=%.3f aud=%.3f pts=%.3f gap=%.3f delay=%.3f",
                         avDiff, videoClockNow - audioClockNow, videoClockNow,
                         audioClockNow, framePtsSec, ptsGap, targetDelay);
                if (rn > 3) s_lastAvDiffLogUs = nowUsForLog;
            }
        } else {
            if (m_setptsZero) {
                targetDelay = 0.001;
            } else {
                if (targetDelay < 0.010) targetDelay = 0.010;
                if (targetDelay > 0.100) targetDelay = 0.100;
            }
        }

        // 6. frameTimer init / re-anchor (compute targetTime, don't advance yet)
        if (m_setptsZero && !bHasAudio) {
            targetTime = nowSec;
        } else if (m_frameTimer <= 0.0 || m_frameTimer < nowSec - 1.0) {
            targetTime = nowSec + targetDelay;
        } else if (nowSec - m_frameTimer > 0.1) {
            targetTime = nowSec;
        } else {
            targetTime = m_frameTimer + targetDelay;
        }

        // 7. wait gate
        double actualDelaySec = targetTime - nowSec;

        if (actualDelaySec > 0.001) {
            int64_t sleepUs = static_cast<int64_t>(actualDelaySec * AV_TIME_BASE);
            if (sleepUs > 20000) sleepUs = 20000;
            if (sleepUs > 500)
                av_usleep((unsigned)sleepUs);
            return;
        }

        // 8. Lateness single-frame drop
        if (m_clock->hasAudio()) {
            int64_t overdueUs = (actualDelaySec < 0.0)
                ? static_cast<int64_t>(-actualDelaySec * AV_TIME_BASE)
                : 0;
            m_stats->lastLatenessUs = overdueUs;

            {
                int64_t curMax = m_stats->maxLatenessUs.load(std::memory_order_acquire);
                while (overdueUs > curMax) {
                    if (m_stats->maxLatenessUs.compare_exchange_weak(
                            curMax, overdueUs,
                            std::memory_order_release, std::memory_order_acquire)) {
                        break;
                    }
                }
            }

            if (overdueUs > DROP_THRESHOLD_US
                && (nowUs - m_lastDropUs) > MIN_DROP_INTERVAL_US) {
                m_frameQueue->discardAndAdvance();
                m_stats->framesDropped++;
                m_lastDropUs = nowUs;
                m_frameTimer = targetTime;

                int64_t curBurst = m_stats->renderSkipBurst.load(std::memory_order_acquire);
                int64_t burstCount = 1;
                while (burstCount > curBurst) {
                    if (m_stats->renderSkipBurst.compare_exchange_weak(
                            curBurst, burstCount,
                            std::memory_order_release, std::memory_order_acquire)) {
                        break;
                    }
                }

                LOG_INFO("Drop frame pts=%.3fs overdue=%lldus delay=%.3f",
                         framePtsSec, (long long)overdueUs, targetDelay);
                return;
            }
        }
    }

render:
    {
        int64_t rpts = m_frameQueue->peekDisplayPts();
        if (rpts < 0) return;

        AVFrame* frame = m_frameQueue->displayFrame();
        if (!frame || !frame->data[0]) {
            m_frameQueue->advanceDisplay();
            return;
        }

        int64_t displayBeforeUs = av_gettime_relative();
        IRenderer* r = renderer();
        if (r) {
            r->displayFrame(frame);
        }
        int64_t displayAfterUs = av_gettime_relative();
        m_stats->recordPaintLatency(displayAfterUs - displayBeforeUs);

        double framePtsSec = rpts / (double)AV_TIME_BASE;
        m_clock->setVideoClock(framePtsSec);
        m_frameQueue->advanceDisplay();
        m_stats->framesRendered++;
        m_stats->frameId++;
        m_frameLastPts = framePtsSec;

        if (m_fastCatchUp) {
            m_fastCatchUpFrameCount++;
        }

        int64_t renderNowUs = av_gettime_relative();
        double presentNow = renderNowUs / (double)AV_TIME_BASE;

        if (m_setptsZero && !m_clock->hasAudio())
            m_frameTimer = presentNow;
        else if (m_frameTimer <= 0.0)
            m_frameTimer = presentNow + DEFAULT_FRAME_DURATION;
        else
            m_frameTimer = targetTime;

        // 10. Render cost compensation
        if (presentNow - m_frameTimer > 0.02)
            m_frameTimer = presentNow;

        if (m_lastRenderUs > 0) {
            m_stats->recordPaintInterval(renderNowUs - m_lastRenderUs);
        }
        m_lastRenderUs = renderNowUs;

        auto rn = m_stats->framesRendered.load();
        if (rn <= 3 || rn % 30 == 0) {
            if (rn % 30 == 0) {
                int vqPeakMs = 0, vqPeakPkts = 0;
                m_videoQueue->drainPeak(vqPeakMs, vqPeakPkts);
                m_stats->recordQueueDepth(vqPeakMs, m_frameQueue->count() * 33);
                m_stats->videoQueuePeakPkts.store(vqPeakPkts, std::memory_order_relaxed);
                // Track peak frame-queue slot usage for CSV diagnostics
                {
                    int curSlots = m_frameQueue->count();
                    int prev = m_stats->frameQueuePeakSlots.load(std::memory_order_relaxed);
                    if (curSlots > prev) {
                        m_stats->frameQueuePeakSlots.store(curSlots, std::memory_order_relaxed);
                    }
                }
                if (auto* rb = m_lifecycle->audioRingBuffer()) {
                    int fillBytes = 0, readEmpty = 0, writeBlocked = 0;
                    rb->snapshotRingCounters(fillBytes, readEmpty, writeBlocked);
                    m_stats->audioRingFillBytes.store(fillBytes, std::memory_order_relaxed);
                    m_stats->audioRingReadEmpty.store(readEmpty, std::memory_order_relaxed);
                    m_stats->audioRingWriteBlocked.store(writeBlocked, std::memory_order_relaxed);
                }
            }
            LOG_INFO("Render #%lld pts=%.3fs overdue=%lldus",
                     (long long)rn, framePtsSec,
                     (long long)m_stats->lastLatenessUs.load());
        }
    }
}

