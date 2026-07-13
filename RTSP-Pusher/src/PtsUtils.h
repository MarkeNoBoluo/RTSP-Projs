#pragma once

#include <cstdint>
#include <algorithm>

// Convert wall-clock microseconds to encoder PTS in {1, 90000} time_base units.
// Built-in monotonic guard: returned PTS is always strictly > lastPts.
//
// captureUs:      wall-clock capture time in microseconds
// serialStartUs:  wall-clock of the first frame in this serial
// lastPts:        [in/out] PTS of the previous frame; initialize to -1
inline int64_t wallClockToVideoPts(int64_t captureUs, int64_t serialStartUs,
                                    int64_t& lastPts) {
    int64_t elapsedUs = captureUs - serialStartUs;
    if (elapsedUs < 0) elapsedUs = 0;
    // time_base {1, 90000}: 1 PTS unit = 1/90000 s
    // pts = elapsedUs(μs) * 90000 / 1,000,000 = elapsedUs * 9 / 100
    int64_t pts = elapsedUs * 9LL / 100LL;
    if (pts <= lastPts) {
        pts = lastPts + 1;
    }
    lastPts = pts;
    return pts;
}
