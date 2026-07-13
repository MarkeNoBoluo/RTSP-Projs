#include "PusherStateMachine.h"

PusherState PusherStateMachine::state() const {
    return static_cast<PusherState>(m_state.load(std::memory_order_acquire));
}

bool PusherStateMachine::transition(PusherState from, PusherState to) {
    int expected = static_cast<int>(from);
    return m_state.compare_exchange_strong(expected, static_cast<int>(to),
                                           std::memory_order_acq_rel);
}

void PusherStateMachine::forceState(PusherState s) {
    m_state.store(static_cast<int>(s), std::memory_order_release);
}
