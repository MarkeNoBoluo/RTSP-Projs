#pragma once

#include "Common.h"
#include <atomic>

class PlayerStateMachine {
public:
    PlayerState state() const;
    bool        transition(PlayerState from, PlayerState to);
    void        forceState(PlayerState s);

private:
    std::atomic<int> m_state{static_cast<int>(PlayerState::Stopped)};
};
