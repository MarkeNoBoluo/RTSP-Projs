#include "PlayerStateMachine.h"

PlayerState PlayerStateMachine::state() const {
    return static_cast<PlayerState>(m_state.load(std::memory_order_acquire));
}

bool PlayerStateMachine::transition(PlayerState from, PlayerState to) {
    int expected = static_cast<int>(from);
    return m_state.compare_exchange_strong(expected, static_cast<int>(to),
                                           std::memory_order_acq_rel);
}

void PlayerStateMachine::forceState(PlayerState s) {
    m_state.store(static_cast<int>(s), std::memory_order_release);
}
