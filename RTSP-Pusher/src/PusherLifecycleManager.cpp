#include "PusherLifecycleManager.h"
#include "RTSPusher.h"
#include "PusherStats.h"
#include "logger/Logger.h"
#include <SDL.h>
#include <algorithm>

extern "C" {
#include <libavutil/time.h>
}

PusherLifecycleManager::PusherLifecycleManager(RTSPusher* pusher)
    : m_pusher(pusher) {}

PusherLifecycleManager::~PusherLifecycleManager() {}

int PusherLifecycleManager::nextSerial() {
    return ++m_serial;
}

void PusherLifecycleManager::scheduleReconnect(const char* reason) {
    bool expected = false;
    if (!m_reconnectPending.compare_exchange_strong(expected, true)) {
        return; // already pending
    }

    LOG_WARN("[lifecycle] Reconnect scheduled: %s (backoff=%ds)", reason, m_backoffSec.load());

    // Push SDL user event to wake up main thread
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = EVENT_RECONNECT;
    event.user.data1 = this;
    SDL_PushEvent(&event);
}

void PusherLifecycleManager::doReconnect() {
    m_reconnectPending = false;

    int backoff = m_backoffSec.load();
    m_reconnectCount++;
    m_pusher->stats()->reconnectCount = m_reconnectCount.load();

    LOG_WARN("[lifecycle] Reconnecting (attempt %d, backoff=%ds)...",
             m_reconnectCount.load(), backoff);

    // 1. Stop pipeline
    LOG_INFO("[lifecycle] Shutting down pipeline...");
    m_pusher->stopPipeline();

    // 2. Increment serial — invalidates all old packets
    int newSerial = nextSerial();
    LOG_INFO("[lifecycle] Serial incremented to %d", newSerial);

    // 3. Exponential backoff
    if (backoff > 0) {
        LOG_INFO("[lifecycle] Waiting %ds before reconnect...", backoff);
        av_usleep(backoff * 1000000u);
    }

    // 4. Restart pipeline
    LOG_INFO("[lifecycle] Restarting pipeline...");
    if (!m_pusher->startPipeline()) {
        LOG_ERROR("[lifecycle] Restart failed");
        if (m_errorCallback) {
            m_errorCallback("reconnect failed");
        }
        return;
    }

    // 5. Update backoff
    m_backoffSec = std::min(backoff * 2, kMaxBackoffSec);

    LOG_INFO("[lifecycle] Reconnect complete, backoff=%ds", m_backoffSec.load());
}

void PusherLifecycleManager::handleReconnectEvent(void* data) {
    auto* mgr = static_cast<PusherLifecycleManager*>(data);
    if (mgr) {
        mgr->doReconnect();
    }
}

void PusherLifecycleManager::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}
