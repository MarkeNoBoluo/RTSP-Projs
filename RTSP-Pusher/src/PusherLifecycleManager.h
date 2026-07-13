#pragma once

#include "Common.h"
#include <atomic>
#include <functional>

class RTSPusher;

class PusherLifecycleManager {
public:
    using ErrorCallback = std::function<void(const char*)>;

    explicit PusherLifecycleManager(RTSPusher* pusher);
    ~PusherLifecycleManager();

    // Called from ANY thread when an error requires reconnect
    void scheduleReconnect(const char* reason);

    // Called from MAIN thread (SDL event loop)
    void doReconnect();

    // Called from main thread with SDL user event data
    static void handleReconnectEvent(void* data);

    int serial() const { return m_serial.load(); }
    int nextSerial();

    void setErrorCallback(ErrorCallback cb);
    int  reconnectCount() const { return m_reconnectCount.load(); }

private:
    RTSPusher* m_pusher;

    std::atomic<int> m_serial{0};
    std::atomic<int> m_reconnectCount{0};
    std::atomic<int> m_backoffSec{1};
    std::atomic<bool> m_reconnectPending{false};

    ErrorCallback m_errorCallback;

    static constexpr int kMaxBackoffSec = 8;
};
