#include "AudioEncodeThread.h"
#include "AudioRingBuffer.h"
#include "EncodedPacketQueue.h"
#include "logger/Logger.h"
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

AudioEncodeThread::AudioEncodeThread(const PusherConfig* config,
                                     AudioRingBuffer* ringBuffer,
                                     EncodedPacketQueue* outputQueue,
                                     PusherStats* stats)
    : m_config(config), m_ringBuffer(ringBuffer), m_outputQueue(outputQueue), m_stats(stats) {}

AudioEncodeThread::~AudioEncodeThread() {
    stop();
}

bool AudioEncodeThread::openEncoder() {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        LOG_ERROR("[audio-encode] AAC encoder not found");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("[audio-encode] avcodec_alloc_context3 failed");
        return false;
    }

    m_codecCtx->sample_rate    = m_config->audioSampleRate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&m_codecCtx->ch_layout, m_config->audioChannels);
#else
    m_codecCtx->channels       = m_config->audioChannels;
    m_codecCtx->channel_layout = av_get_default_channel_layout(m_config->audioChannels);
#endif
    m_codecCtx->sample_fmt     = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    m_codecCtx->bit_rate       = m_config->audioBitrate * 1000;  // config in kbps, FFmpeg expects bps
    m_codecCtx->time_base      = {1, m_config->audioSampleRate};
    m_codecCtx->flags         |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (m_codecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        m_codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("[audio-encode] avcodec_open2 failed: %s", errBuf);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // Determine input frame size (samples per channel per frame)
    m_inputSampleCount = m_codecCtx->frame_size > 0
                         ? m_codecCtx->frame_size
                         : 1024; // AAC typically 1024

    LOG_INFO("[audio-encode] AAC encoder opened: %dHz %dch %dkbps  frame_size=%d  fmt=%d",
             m_config->audioSampleRate, m_config->audioChannels,
             m_config->audioBitrate, m_inputSampleCount, m_codecCtx->sample_fmt);
    return true;
}

bool AudioEncodeThread::ensureResampler(const SDL_AudioSpec& inputSpec) {
    if (m_swrCtx &&
        inputSpec.freq == m_srcSampleRate &&
        inputSpec.channels == m_srcChannels &&
        (int)inputSpec.format == m_srcFormat) {
        return true;
    }

    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }

    AVSampleFormat srcFmt;
    switch (inputSpec.format) {
        case AUDIO_S16SYS: srcFmt = AV_SAMPLE_FMT_S16;  break;
        case AUDIO_S32SYS: srcFmt = AV_SAMPLE_FMT_S32;  break;
        case AUDIO_F32SYS: srcFmt = AV_SAMPLE_FMT_FLT;  break;
        case AUDIO_U8:     srcFmt = AV_SAMPLE_FMT_U8;   break;
        default:
            LOG_WARN("[audio-encode] Unsupported SDL format 0x%x, assuming S16",
                     inputSpec.format);
            srcFmt = AV_SAMPLE_FMT_S16;
            break;
    }

    m_srcSampleRate = inputSpec.freq;
    m_srcChannels   = inputSpec.channels;
    m_srcFormat     = inputSpec.format;

#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout srcLayout, dstLayout;
    av_channel_layout_default(&srcLayout, m_srcChannels);
    av_channel_layout_default(&dstLayout, m_config->audioChannels);

    SwrContext* swr = nullptr;
    int sr = swr_alloc_set_opts2(&swr,
                                   &dstLayout, m_codecCtx->sample_fmt,
                                   m_config->audioSampleRate,
                                   &srcLayout, srcFmt, m_srcSampleRate,
                                   0, nullptr);
    av_channel_layout_uninit(&srcLayout);
    av_channel_layout_uninit(&dstLayout);

    if (sr < 0 || !swr) {
        LOG_ERROR("[audio-encode] swr_alloc_set_opts2 failed: %d", sr);
        return false;
    }
    m_swrCtx = swr;
#else
    int64_t srcLayout = av_get_default_channel_layout(m_srcChannels);
    int64_t dstLayout = av_get_default_channel_layout(m_config->audioChannels);

    m_swrCtx = swr_alloc_set_opts(nullptr,
                                   dstLayout, m_codecCtx->sample_fmt,
                                   m_config->audioSampleRate,
                                   srcLayout, srcFmt, m_srcSampleRate,
                                   0, nullptr);
    if (!m_swrCtx) {
        LOG_ERROR("[audio-encode] swr_alloc_set_opts failed");
        return false;
    }
#endif

    int ret = swr_init(m_swrCtx);
    if (ret < 0) {
        LOG_ERROR("[audio-encode] swr_init failed");
        swr_free(&m_swrCtx);
        return false;
    }

    LOG_INFO("[audio-encode] Resampler: %dHz/%s/%dch -> %dHz/%d/%dch",
             m_srcSampleRate, av_get_sample_fmt_name(srcFmt), m_srcChannels,
             m_config->audioSampleRate, m_codecCtx->sample_fmt, m_config->audioChannels);
    return true;
}

bool AudioEncodeThread::start(const SDL_AudioSpec& inputSpec, int serial) {
    m_serial = serial;
    m_abort = false;

    if (!openEncoder()) return false;
    if (!ensureResampler(inputSpec)) {
        closeEncoder();
        return false;
    }

    // Allocate input frame (to be filled with PCM from ring buffer)
    m_inputFrame = av_frame_alloc();
    if (!m_inputFrame) {
        closeEncoder();
        return false;
    }

    // Allocate resampled frame
    m_resampledFrame = av_frame_alloc();
    if (!m_resampledFrame) {
        av_frame_free(&m_inputFrame);
        closeEncoder();
        return false;
    }
    m_resampledFrame->format      = m_codecCtx->sample_fmt;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    int cRet = av_channel_layout_copy(&m_resampledFrame->ch_layout, &m_codecCtx->ch_layout);
    if (cRet < 0) {
        LOG_ERROR("[audio-encode] av_channel_layout_copy failed: %d", cRet);
        av_frame_free(&m_inputFrame);
        av_frame_free(&m_resampledFrame);
        closeEncoder();
        return false;
    }
#else
    m_resampledFrame->channel_layout = m_codecCtx->channel_layout;
    m_resampledFrame->channels       = m_codecCtx->channels;
#endif
    m_resampledFrame->sample_rate    = m_codecCtx->sample_rate;
    m_resampledFrame->nb_samples     = m_inputSampleCount;

    int ret = av_frame_get_buffer(m_resampledFrame, 0);
    if (ret < 0) {
        av_frame_free(&m_inputFrame);
        av_frame_free(&m_resampledFrame);
        closeEncoder();
        return false;
    }

    m_outputSampleCount = m_inputSampleCount;
    m_thread = std::thread(&AudioEncodeThread::run, this);
    return true;
}

void AudioEncodeThread::stop() {
    m_abort = true;
    if (m_ringBuffer) m_ringBuffer->flush();
}

void AudioEncodeThread::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    closeEncoder();
}

void AudioEncodeThread::setErrorCallback(ErrorCallback cb) {
    m_errorCallback = std::move(cb);
}

void AudioEncodeThread::setSerial(int serial) {
    m_serial = serial;
}

void AudioEncodeThread::run() {
    LOG_INFO("[audio-encode] Thread started, serial=%d", m_serial.load());

    int64_t startUs = av_gettime_relative();
    int64_t lastStatsUs = startUs;
    int64_t nextPts = 0; // cumulative input samples for encoder PTS
    int64_t encodeCount = 0;
    int64_t underrunCount = 0;
    int64_t lastAudioPtsMs = -1;
    int serial = m_serial.load();

    // Buffer for reading from ring buffer
    int bytesPerSample = av_get_bytes_per_sample(m_codecCtx->sample_fmt);
    int bytesPerInputSample = m_srcChannels *
        (SDL_AUDIO_BITSIZE(m_srcFormat) / 8);
    int bytesPerFrame = m_inputSampleCount * bytesPerInputSample;

    // Input buffer (from ring)
    uint8_t* pcmBuf = new uint8_t[bytesPerFrame];

    while (!m_abort) {
        int currentSerial = m_serial.load();
        if (currentSerial != serial) {
            serial = currentSerial;
            nextPts = 0;
            if (m_codecCtx) avcodec_flush_buffers(m_codecCtx);
        }

        int filled = 0;
        bool countedUnderrun = false;
        int64_t emptyStartUs = 0;
        while (!m_abort && filled < bytesPerFrame) {
            int read = m_ringBuffer->read(pcmBuf + filled, bytesPerFrame - filled);
            if (read > 0) {
                filled += read;
                emptyStartUs = 0;
                continue;
            }

            int64_t nowUs = av_gettime_relative();
            if (emptyStartUs == 0) {
                emptyStartUs = nowUs;
            } else if (!countedUnderrun && nowUs - emptyStartUs > 20000) {
                underrunCount++;
                m_stats->audioUnderrunCount = underrunCount;
                countedUnderrun = true;
            }
            av_usleep(2000);
        }
        if (m_abort) {
            break;
        }

        // Fill input AVFrame from PCM buffer
        // Note: SDL interleaves channels, so we fill plane 0
        AVSampleFormat srcFmt;
        switch (m_srcFormat) {
            case AUDIO_S16SYS: srcFmt = AV_SAMPLE_FMT_S16; break;
            case AUDIO_S32SYS: srcFmt = AV_SAMPLE_FMT_S32; break;
            case AUDIO_F32SYS: srcFmt = AV_SAMPLE_FMT_FLT; break;
            default: srcFmt = AV_SAMPLE_FMT_S16; break;
        }
        int srcBufSize = bytesPerFrame;
        int srcSamples = srcBufSize / (m_srcChannels * av_get_bytes_per_sample(srcFmt));

        // Use packed audio buffer as source for resampling
        const uint8_t* srcData = pcmBuf;

        // Resample
        const uint8_t* srcPtr = srcData;
        int outSamples = swr_convert(m_swrCtx,
                                      m_resampledFrame->data,
                                      m_outputSampleCount,
                                      &srcPtr,
                                      srcSamples);
        if (outSamples < 0) {
            LOG_WARN("[audio-encode] swr_convert error: %d", outSamples);
            continue;
        }

        m_resampledFrame->nb_samples = outSamples;
        m_resampledFrame->pts = nextPts;

        // Encode
        int ret = avcodec_send_frame(m_codecCtx, m_resampledFrame);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            continue;
        }
        if (ret >= 0) {
            nextPts += outSamples;
        }

        // Receive encoded packets
        while (true) {
            AVPacket* pkt = av_packet_alloc();
            if (!pkt) break;

            ret = avcodec_receive_packet(m_codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&pkt);
                break;
            }
            if (ret < 0) {
                av_packet_free(&pkt);
                break;
            }

            encodeCount++;

            // Record first audio encode wall-clock
            if (m_stats->firstAudioCaptureUs.load() == 0) {
                m_stats->firstAudioCaptureUs = av_gettime_relative();
            }

            // Compute audio PTS in ms for A/V sync observation
            // Use media time (accumulated samples / sampleRate) — matches encoder PTS
            int64_t audioMs = (nextPts * 1000LL) / m_config->audioSampleRate;
            if (audioMs < lastAudioPtsMs && lastAudioPtsMs >= 0) {
                m_stats->ptsErrorCount++;
            }
            lastAudioPtsMs = audioMs;
            m_stats->audioPtsMs = audioMs;

            EncodedPacket ep;
            ep.pkt = pkt;
            ep.streamIndex = 1; // audio
            ep.serial = serial;
            ep.timeBase = m_codecCtx->time_base;

            bool enqueued = m_outputQueue->push(std::move(ep));
            if (enqueued) {
                m_stats->audioFramesEncoded++;
            }
            m_stats->encodedQueueDepth = m_outputQueue->size();
        }

        // Periodic stats
        int64_t nowUs = av_gettime_relative();
        if (nowUs - lastStatsUs > 5000000) {
            double elapsedSec = (nowUs - startUs) / 1000000.0;
            double encFps = elapsedSec > 0 ? encodeCount / elapsedSec : 0;
            int ringAvail = m_ringBuffer->available();
            m_stats->audioRingBytes = ringAvail;
            m_stats->audioOverflowCount = m_ringBuffer->overflowCount();
            m_stats->audioUnderrunCount = underrunCount;
            LOG_INFO("[audio-encode] encFps=%.1f  frames=%lld  ringBytes=%d  "
                     "overflow=%d  underrun=%lld  nextPts=%lld",
                     encFps, (long long)encodeCount, ringAvail,
                     m_ringBuffer->overflowCount(), (long long)underrunCount,
                     (long long)nextPts);
            lastStatsUs = nowUs;
        }
    }

    delete[] pcmBuf;

    int64_t endUs = av_gettime_relative();
    LOG_INFO("[audio-encode] Thread stopped: frames=%lld  duration=%.1fs",
             (long long)encodeCount, (endUs - startUs) / 1000000.0);
}

void AudioEncodeThread::closeEncoder() {
    if (m_inputFrame) {
        av_frame_free(&m_inputFrame);
        m_inputFrame = nullptr;
    }
    if (m_resampledFrame) {
        av_frame_free(&m_resampledFrame);
        m_resampledFrame = nullptr;
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}
