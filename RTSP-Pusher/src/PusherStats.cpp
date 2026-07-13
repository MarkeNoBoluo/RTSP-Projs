#include "PusherStats.h"
#include <cstdio>
#include <ctime>

PusherStats* PusherStats::s_globalInstance = nullptr;

void PusherStats::reset() {
    videoFramesCaptured = 0;
    videoFramesDropped = 0;
    videoFramesEncoded = 0;
    audioBytesCaptured = 0;
    audioFramesEncoded = 0;
    videoPacketCount = 0;
    audioPacketCount = 0;
    packetsWritten = 0;
    writeErrorCount = 0;
    videoRawQueueDepth = 0;
    encodedQueueDepth = 0;
    windowRawQueueMax = 0;
    windowEncQueueMax = 0;
    bitrateKbps = 0;
    windowEncodeMaxUs = 0;
    windowMuxWriteMaxUs = 0;
    vbvUnderflowCount = 0;
    audioRingBytes = 0;
    audioOverflowCount = 0;
    audioUnderrunCount = 0;
    pipelineStartUs = 0;
    firstVideoCaptureUs = 0;
    firstAudioCaptureUs = 0;
    videoPtsMs = 0;
    audioPtsMs = 0;
    avOffsetMs = 0;
    ptsErrorCount = 0;
    // reconnectCount intentionally NOT reset — it accumulates across reconnects
}

void PusherStats::writeCsvHeader(FILE* f) {
    fprintf(f, "timestamp,videoCaptured,videoDropped,videoEncoded,audioBytes,audioEncoded,"
            "videoPackets,audioPackets,packetsWritten,writeErrors,reconnects,"
            "rawQueue,rawQMax,encQueue,encQMax,"
            "bitrateKbps,encMaxUs,muxWriteMaxUs,"
            "vbvUnderflow,audioRingBytes,audioOverflow,audioUnderrun,"
            "videoPtsMs,audioPtsMs,avOffsetMs,ptsErrorCount,"
            "firstVideoCaptureUs,firstAudioCaptureUs\n");
    fflush(f);
}

void PusherStats::writeCsvRow(FILE* f) {
    if (!f) return;
    fprintf(f, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%d,%d,%d,%d,%lld,%lld,%lld,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
            (long long)time(nullptr),
            (long long)videoFramesCaptured.load(),
            (long long)videoFramesDropped.load(),
            (long long)videoFramesEncoded.load(),
            (long long)audioBytesCaptured.load(),
            (long long)audioFramesEncoded.load(),
            (long long)videoPacketCount.load(),
            (long long)audioPacketCount.load(),
            (long long)packetsWritten.load(),
            (long long)writeErrorCount.load(),
            (long long)reconnectCount.load(),
            videoRawQueueDepth.load(),
            windowRawQueueMax.load(),
            encodedQueueDepth.load(),
            windowEncQueueMax.load(),
            bitrateKbps.load(),
            (long long)windowEncodeMaxUs.load(),
            (long long)windowMuxWriteMaxUs.load(),
            (long long)vbvUnderflowCount.load(),
            audioRingBytes.load(),
            (long long)audioOverflowCount.load(),
            (long long)audioUnderrunCount.load(),
            (long long)videoPtsMs.load(),
            (long long)audioPtsMs.load(),
            (long long)avOffsetMs.load(),
            (long long)ptsErrorCount.load(),
            (long long)firstVideoCaptureUs.load(),
            (long long)firstAudioCaptureUs.load());
    fflush(f);
}
