#pragma once

#include "Common.h"
#include <atomic>

class PusherStateMachine {
public:
    PusherState state() const;
    bool        transition(PusherState from, PusherState to);
    void        forceState(PusherState s);

private:
    std::atomic<int> m_state{static_cast<int>(PusherState::Stopped)};
};
