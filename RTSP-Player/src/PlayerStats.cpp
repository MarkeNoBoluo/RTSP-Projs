#include "PlayerStats.h"
#include <ctime>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

bool PlayerStats::initCsv(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    m_csvFile.open(path, std::ios::app);
    if (!m_csvFile.is_open()) return false;

    // session ID: YYYYMMDD_HHMMSS
    time_t now = time(nullptr);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
    m_sessionId = buf;
    return true;
}

void PlayerStats::writeCsvRow() {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (!m_csvFile.is_open()) return;

    if (!m_csvHeaderWritten) {
        csvWrite("session,elapsed_s,"
                 "vFrmDec,vFrmRend,vFrmDrop,vDropRate_pct,"
                 "aPktIn,aFrmDec,aBytesWrit,"
                 "vQPeakMs,vQPeakPkt,aQPeakMs,"
                 "latAvgMs,latMaxMs,skipBurst,"
                 "vDecSendMaxUs,vDecRecvMaxUs,vDecErr,"
                 "fqFail,fqOverwrt,fqPeakSlots,"
                 "aUnder,aOver,aRingFillB,aRingEmptyN,aRingBlockN,"
                 "rndIntvAvgMs,rndIntvMaxMs,rndLatAvgMs,rndLatMaxMs,"
                 "reconn,reconMs,"
                 "v1stDecUs,v1stRendUs,a1stDecUs,a1stPlayUs,"
                 "frameId,"
                 "vPopTO,catDrop,vStall,audDiffMs,clkDiffMs,"
                 "hwDecEnabled,hwDecFrames,hwXferFail,hwXferMaxUs");
        m_csvHeaderWritten = true;
    }

    int64_t dec   = framesDecoded.load();
    int64_t ren   = framesRendered.load();
    int64_t drp   = framesDropped.load();
    int   dropPct = (dec > 0) ? (int)(drp * 100 / dec) : 0;

    int64_t aPkt   = audioPacketsReceived.load();
    int64_t aFrm   = audioFramesDecoded.load();
    int64_t aBytes = audioBytesWritten.load();

    // exchange(0) atomically reads and resets, eliminating the race window
    // between the old load() + later =0 that lost increments from other threads
    int vqMs  = videoQueuePeakMs.exchange(0);
    int vqPk  = videoQueuePeakPkts.exchange(0);
    int aqMs  = audioQueuePeakMs.exchange(0);

    int64_t lat    = lastLatenessUs.load() / 1000;  // not reset; carries over
    int64_t latMax = maxLatenessUs.exchange(0) / 1000;
    int64_t burst  = renderSkipBurst.exchange(0);

    int64_t dsMax = decodeSendUsMax.exchange(0);
    int64_t drMax = decodeReceiveUsMax.exchange(0);
    int     deErr = decodeErrorCount.exchange(0);

    int fqFail = frameQueueWriteFailures.exchange(0);
    int fqOver = frameQueueOverwrites.exchange(0);
    int fqPeak = frameQueuePeakSlots.exchange(0);

    int au    = audioUnderruns.exchange(0);
    int ao    = audioOverruns.exchange(0);
    int aFill = audioRingFillBytes.exchange(0);
    int aEmpt = audioRingReadEmpty.exchange(0);
    int aBlk  = audioRingWriteBlocked.exchange(0);

    int64_t pivAvg = 0, pivMax = 0;
    int pc = paintIntervalCount.load();
    if (pc > 0) {
        pivAvg = paintIntervalSumUs.load() / pc / 1000;
        pivMax = paintIntervalMaxUs.load() / 1000;
    }
    int64_t plAvg = 0, plMax = 0;
    int pl = paintLatencyCount.load();
    if (pl > 0) {
        plAvg = paintLatencySumUs.load() / pl / 1000;
        plMax = paintLatencyMaxUs.load() / 1000;
    }

    int     rec   = reconnectCount.load();
    int64_t recMs = totalReconnectMs.load();

    int64_t vfdUs = videoFirstDecodeUs.load();
    int64_t vfrUs = videoFirstRenderUs.load();
    int64_t afdUs = audioFirstDecodeUs.load();
    int64_t afpUs = audioFirstPlayUs.load();

    int vpt = videoPopTimeouts.exchange(0);
    int cdr = catchUpDrops.exchange(0);
    int vst = videoStallCount.exchange(0);
    int64_t adm = frameAudDiffMs.exchange(0);
    int64_t cdm = clockDiffMs.exchange(0);

    bool    hwEn   = hwDecodeEnabled.load();
    int64_t hwDec  = hwDecodedFrames.load();
    int     hwFail = hwTransferFailures.exchange(0);
    int64_t hwMax  = hwTransferMaxUs.exchange(0);

    uint64_t fid  = frameId.load();
    int64_t  elap = static_cast<int64_t>(time(nullptr));

    std::ostringstream ss;
    ss << m_sessionId << ',' << elap << ','
       << dec << ',' << ren << ',' << drp << ',' << dropPct << ','
       << aPkt << ',' << aFrm << ',' << aBytes << ','
       << vqMs << ',' << vqPk << ',' << aqMs << ','
       << lat << ',' << latMax << ',' << burst << ','
       << dsMax << ',' << drMax << ',' << deErr << ','
       << fqFail << ',' << fqOver << ',' << fqPeak << ','
       << au << ',' << ao << ',' << aFill << ',' << aEmpt << ',' << aBlk << ','
       << pivAvg << ',' << pivMax << ','
       << plAvg << ',' << plMax << ','
       << rec << ',' << recMs << ','
       << vfdUs << ',' << vfrUs << ',' << afdUs << ',' << afpUs << ','
       << fid << ','
       << vpt << ',' << cdr << ',' << vst << ',' << adm << ',' << cdm
       << ',' << hwEn << ',' << hwDec << ',' << hwFail << ',' << hwMax;

    csvWrite(ss.str());

    // Reset remaining per-interval accumulators
    // (all above counters were atomically read-and-cleared via exchange(0))
    paintIntervalMinUs.store(0);
    paintIntervalMaxUs.store(0);
    paintIntervalSumUs.store(0);
    paintIntervalCount.store(0);
    paintLatencyMinUs.store(0);
    paintLatencyMaxUs.store(0);
    paintLatencySumUs.store(0);
    paintLatencyCount.store(0);
}

void PlayerStats::closeCsv() {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (m_csvFile.is_open()) {
        m_csvFile.close();
    }
}

void PlayerStats::csvWrite(const std::string& line) {
    m_csvFile << line << '\n';
    m_csvFile.flush();
}

void PlayerStats::recordPaintInterval(int64_t us) {
    int cnt = paintIntervalCount.fetch_add(1) + 1;
    paintIntervalSumUs.fetch_add(us);
    int64_t cur = paintIntervalMinUs.load();
    if (cur == 0 || us < cur) paintIntervalMinUs.store(us);
    cur = paintIntervalMaxUs.load();
    if (us > cur) paintIntervalMaxUs.store(us);
}

void PlayerStats::recordPaintLatency(int64_t us) {
    int cnt = paintLatencyCount.fetch_add(1) + 1;
    paintLatencySumUs.fetch_add(us);
    int64_t cur = paintLatencyMinUs.load();
    if (cur == 0 || us < cur) paintLatencyMinUs.store(us);
    cur = paintLatencyMaxUs.load();
    if (us > cur) paintLatencyMaxUs.store(us);
}

void PlayerStats::recordQueueDepth(int videoMs, int audioMs) {
    int cur = videoQueuePeakMs.load();
    if (videoMs > cur) videoQueuePeakMs.store(videoMs);
    cur = audioQueuePeakMs.load();
    if (audioMs > cur) audioQueuePeakMs.store(audioMs);
}

void PlayerStats::recordSkipBurstEnd() {
    renderSkipBurst.store(0);
}
