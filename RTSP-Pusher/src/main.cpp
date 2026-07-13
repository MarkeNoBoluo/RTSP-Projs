#define SDL_MAIN_HANDLED
#include <windows.h>
#include <conio.h>
#include <SDL.h>
#include <vector>
#include <algorithm>

extern "C" {
__declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
__declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
}

// ── Monitor enumeration ─────────────────────────────────────────
struct MonitorEntry {
    int  index;
    int  x, y;               // virtual-screen position (logical, post-DPI-scaling)
    int  width, height;      // logical resolution (= physical / scale)
    int  physWidth, physHeight; // physical pixel resolution
    bool isPrimary;
    char deviceName[32];
};

static std::vector<MonitorEntry> enumerateMonitors() {
    std::vector<MonitorEntry> result;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT rc, LPARAM lp) -> BOOL {
            auto* vec = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
            MONITORINFOEX mi;
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfo(hMon, &mi)) return TRUE;

            // Get physical resolution via EnumDisplaySettings
            DEVMODE dm;
            ZeroMemory(&dm, sizeof(dm));
            dm.dmSize = sizeof(dm);
            int physW = 0, physH = 0;
            if (EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                physW = dm.dmPelsWidth;
                physH = dm.dmPelsHeight;
            }

            MonitorEntry e;
            e.x          = mi.rcMonitor.left;
            e.y          = mi.rcMonitor.top;
            e.width      = mi.rcMonitor.right - mi.rcMonitor.left;
            e.height     = mi.rcMonitor.bottom - mi.rcMonitor.top;
            e.physWidth  = physW > 0 ? physW : e.width;
            e.physHeight = physH > 0 ? physH : e.height;
            e.isPrimary  = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            snprintf(e.deviceName, sizeof(e.deviceName), "%s", mi.szDevice);
            vec->push_back(e);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));

    // Sort: primary first, then left-to-right, top-to-bottom
    std::sort(result.begin(), result.end(),
        [](const MonitorEntry& a, const MonitorEntry& b) {
            if (a.isPrimary != b.isPrimary) return a.isPrimary;
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });
    for (size_t i = 0; i < result.size(); ++i) {
        result[i].index = static_cast<int>(i) + 1;  // 1-based for user display
    }
    return result;
}

#include "RTSPusher.h"
#include "PusherConfig.h"
#include "PusherLifecycleManager.h"
#include "PusherStats.h"
#include "HardwareEncoderDetector.h"
#include "logger/Logger.h"
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
}

static void ffmpegLogCallback(void*, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    logger::Logger::instance().log(logger::Level::Debug, "ffmpeg", 0, "%s", buf);

    // Detect VBV underflow from x264 log messages
    if (strstr(buf, "VBV underflow")) {
        PusherStats* s = PusherStats::globalInstance();
        if (s) s->vbvUnderflowCount++;
    }
}

static void printUsage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --url <url>            RTSP push URL (default: rtsp://192.168.42.116:25544/live)\n"
        "  --list-screens          List available monitors and exit\n"
        "  --screen <n>            Monitor to capture: 0=all(default), 1=primary, 2,3,...\n"
        "  --capture-size WxH     Capture resolution (default: 2560x1440)\n"
        "  --output-size WxH      Output resolution (default: 1920x1080)\n"
        "  --fps <n>              Capture/encode framerate (default: 30)\n"
        "  --bitrate <n>          Video bitrate in kbps (default: 20000)\n"
        "  --maxrate <n>          Max video bitrate in kbps (default: 20000)\n"
        "  --bufsize <n>          VBV buffer size in kbits (default: 20000)\n"
        "  --crf <n>              CRF quality, 0=ABR mode (default: 0)\n"
        "  --list-audio-devices   List available capture devices and exit\n"
        "  --audio-device <name>  SDL audio capture device name\n"
        "  --audio-device-index <n> SDL audio capture device index from --list-audio-devices\n"
        "  --no-audio             Disable audio capture\n"
        "  --transport <tcp|udp>  RTSP transport (default: tcp)\n"
        "  --hw-encoder <name>    Hardware encoder: auto, qsv, nvenc, off (default: off)\n"
        "  --capture-method <m>   Capture backend: auto, gdigrab, ddagrab (default: auto)\n"
        "  --list-encoders         List available H.264 encoders and exit\n"
        "  --log <path>           Log file path (default: rtsp_pusher.log)\n"
        "  --stats-csv <path>     Stats CSV output path (default: rtsp_pusher_stats.csv)\n"
        "  --duration <seconds>   Auto-exit after N seconds (for test automation)\n"
        "  --help                 Show this help\n"
        "\nExit: press q/ESC in SDL window, send 'q' via stdin, or use --duration.\n",
        prog);
}

int main(int argc, char* argv[]) {
    // Declare DPI awareness so gdigrab captures native (physical) resolution,
    // not the DPI-scaled logical resolution from the composited desktop.
    SetProcessDPIAware();

    // ── Parse command-line arguments ──────────────────────────────
    PusherConfig config;
    const char* logPath     = "rtsp_pusher.log";
    const char* statsCsv    = "rtsp_pusher_stats.csv";
    int         durationSec = 0;  // 0 = run until manual exit
    bool listAudioDevicesOnly = false;
    bool listScreensOnly    = false;
    bool listEncodersOnly   = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--list-audio-devices") == 0) {
            listAudioDevicesOnly = true;
        } else if (std::strcmp(argv[i], "--list-screens") == 0) {
            listScreensOnly = true;
        } else if (std::strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            config.screenIndex = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-screen") == 0 && i + 1 < argc) {
            config.screenIndex = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            config.rtspUrl = argv[++i];
        } else if (std::strcmp(argv[i], "-url") == 0 && i + 1 < argc) {
            config.rtspUrl = argv[++i];
        } else if (std::strcmp(argv[i], "--capture-size") == 0 && i + 1 < argc) {
            int w, h;
            if (sscanf_s(argv[++i], "%dx%d", &w, &h) == 2) {
                config.captureWidth = w;
                config.captureHeight = h;
            } else {
                fprintf(stderr, "Invalid --capture-size format. Use WxH.\n");
            }
        } else if (std::strcmp(argv[i], "-capture-size") == 0 && i + 1 < argc) {
            int w, h;
            if (sscanf_s(argv[++i], "%dx%d", &w, &h) == 2) {
                config.captureWidth = w;
                config.captureHeight = h;
            } else {
                fprintf(stderr, "Invalid -capture-size format. Use WxH.\n");
            }
        } else if (std::strcmp(argv[i], "--output-size") == 0 && i + 1 < argc) {
            int w, h;
            if (sscanf_s(argv[++i], "%dx%d", &w, &h) == 2) {
                config.outputWidth = w;
                config.outputHeight = h;
            } else {
                fprintf(stderr, "Invalid --output-size format. Use WxH.\n");
            }
        } else if (std::strcmp(argv[i], "-output-size") == 0 && i + 1 < argc) {
            int w, h;
            if (sscanf_s(argv[++i], "%dx%d", &w, &h) == 2) {
                config.outputWidth = w;
                config.outputHeight = h;
            } else {
                fprintf(stderr, "Invalid -output-size format. Use WxH.\n");
            }
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            config.captureFps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-fps") == 0 && i + 1 < argc) {
            config.captureFps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            config.videoBitrate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-bitrate") == 0 && i + 1 < argc) {
            config.videoBitrate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--maxrate") == 0 && i + 1 < argc) {
            config.videoMaxrate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-maxrate") == 0 && i + 1 < argc) {
            config.videoMaxrate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--bufsize") == 0 && i + 1 < argc) {
            config.videoBufsize = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-bufsize") == 0 && i + 1 < argc) {
            config.videoBufsize = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            config.crf = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-crf") == 0 && i + 1 < argc) {
            config.crf = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--audio-device") == 0 && i + 1 < argc) {
            config.audioDeviceName = argv[++i];
            config.audioDeviceIndex = -1;
            config.requireAudio = true;
        } else if (std::strcmp(argv[i], "-audio-device") == 0 && i + 1 < argc) {
            config.audioDeviceName = argv[++i];
            config.audioDeviceIndex = -1;
            config.requireAudio = true;
        } else if (std::strcmp(argv[i], "--audio-device-index") == 0 && i + 1 < argc) {
            config.audioDeviceIndex = std::atoi(argv[++i]);
            config.audioDeviceName = nullptr;
            config.requireAudio = true;
        } else if (std::strcmp(argv[i], "-audio-device-index") == 0 && i + 1 < argc) {
            config.audioDeviceIndex = std::atoi(argv[++i]);
            config.audioDeviceName = nullptr;
            config.requireAudio = true;
        } else if (std::strcmp(argv[i], "--no-audio") == 0) {
            config.enableAudio = false;
            config.requireAudio = false;
        } else if (std::strcmp(argv[i], "-no-audio") == 0) {
            config.enableAudio = false;
            config.requireAudio = false;
        } else if (std::strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            config.rtspTransport = argv[++i];
        } else if (std::strcmp(argv[i], "-transport") == 0 && i + 1 < argc) {
            config.rtspTransport = argv[++i];
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        } else if (std::strcmp(argv[i], "-log") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        } else if (std::strcmp(argv[i], "--stats-csv") == 0 && i + 1 < argc) {
            statsCsv = argv[++i];
        } else if (std::strcmp(argv[i], "-stats-csv") == 0 && i + 1 < argc) {
            statsCsv = argv[++i];
        } else if (std::strcmp(argv[i], "--list-encoders") == 0) {
            listEncodersOnly = true;
        } else if (std::strcmp(argv[i], "-list-encoders") == 0) {
            listEncodersOnly = true;
        } else if (std::strcmp(argv[i], "--hw-encoder") == 0 && i + 1 < argc) {
            config.hwEncoder = argv[++i];
        } else if (std::strcmp(argv[i], "-hw-encoder") == 0 && i + 1 < argc) {
            config.hwEncoder = argv[++i];
        } else if (std::strcmp(argv[i], "--capture-method") == 0 && i + 1 < argc) {
            config.captureMethod = argv[++i];
        } else if (std::strcmp(argv[i], "-capture-method") == 0 && i + 1 < argc) {
            config.captureMethod = argv[++i];
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            durationSec = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-duration") == 0 && i + 1 < argc) {
            durationSec = std::atoi(argv[++i]);
        } else {
            fprintf(stderr, "Warning: unrecognized argument '%s'\n", argv[i]);
        }
    }

    // ── Handle --list-encoders (pure query, before any init) ────────
    if (listEncodersOnly) {
        printAvailableEncoders();
        return 0;
    }

    // ── Validate --hw-encoder ────────────────────────────────────────
    {
        const char* resolved = resolveEncoderName(config.hwEncoder);
        if (!resolved) {
            fprintf(stderr, "Unknown encoder '%s'.\n", config.hwEncoder);
            fprintf(stderr, "Use --list-encoders to see available encoders.\n");
            return 1;
        }
    }

    // ── Resolve captureMethod ─────────────────────────────────────
    {
        if (std::strcmp(config.captureMethod, "auto") == 0) {
            // QSV/NVENC encoders prefer ddagrab pipeline; everything else uses gdigrab
            if (std::strcmp(config.hwEncoder, "qsv") == 0 ||
                std::strcmp(config.hwEncoder, "h264_qsv") == 0 ||
                std::strcmp(config.hwEncoder, "nvenc") == 0 ||
                std::strcmp(config.hwEncoder, "h264_nvenc") == 0) {
                config.captureMethod = "ddagrab";
            } else {
                config.captureMethod = "gdigrab";
            }
        }

        // Validate ddagrab: x64 only
        if (std::strcmp(config.captureMethod, "ddagrab") == 0) {
#if !defined(_WIN64)
            fprintf(stderr, "ddagrab capture method requires x64 build.\n");
            return 1;
#endif
        } else if (std::strcmp(config.captureMethod, "gdigrab") != 0) {
            fprintf(stderr, "Unknown --capture-method '%s'. Use gdigrab, ddagrab, or auto.\n",
                    config.captureMethod);
            return 1;
        }
    }

    // ── Resolve screen selection ──────────────────────────────────
    auto monitors = enumerateMonitors();

    if (listScreensOnly) {
        printf("Monitors (DPI-aware: coordinates = physical pixels when DPI=100%%):\n");
        printf("%-4s %-16s %-16s %-16s %s\n",
               "Idx", "Capture (gdigrab)", "Native (phys.)", "Offset", "Flags");
        printf("%-4s %-16s %-16s %-16s %s\n",
               "---", "----------------", "--------------", "------", "-----");
        for (const auto& m : monitors) {
            bool sameAsNative = (m.width == m.physWidth && m.height == m.physHeight);
            printf("[%d]   %5dx%-8d  %5dx%-8d  (%+5d,%+5d)   %s%s\n",
                   m.index,
                   m.width, m.height,
                   m.physWidth, m.physHeight,
                   m.x, m.y,
                   m.isPrimary ? "PRIMARY" : "",
                   m.isPrimary ? "" : (sameAsNative ? "" : "  *"));
        }
        printf("[0]   All monitors (virtual desktop)\n");
        printf("\n  * = DPI != system DPI, gdigrab captures scaled content at listed size.\n");
        return 0;
    }

    if (config.screenIndex > 0) {
        const MonitorEntry* target = nullptr;
        for (const auto& m : monitors) {
            if (m.index == config.screenIndex) {
                target = &m;
                break;
            }
        }
        if (!target) {
            fprintf(stderr, "Screen index %d not found. Use --list-screens to see available monitors.\n",
                    config.screenIndex);
            return 1;
        }
        config.captureOffsetX = target->x;
        config.captureOffsetY = target->y;
        config.captureWidth   = target->width;
        config.captureHeight  = target->height;
    }

    // ── Init logger ───────────────────────────────────────────────
    logger::Logger::instance().initLogFile(logPath);
    atexit([]() { logger::Logger::instance().closeLogFile(); });

    LOG_INFO("RTSP Pusher v1.0 start");

    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(ffmpegLogCallback);

    // ── Init SDL ──────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ── Enumerate audio capture devices ──────────────────────────
    int numCaptureDevices = SDL_GetNumAudioDevices(1);  // 1 = capture
    LOG_INFO("Audio capture devices: %d available", numCaptureDevices);
    for (int i = 0; i < numCaptureDevices; ++i) {
        LOG_INFO("  [%d] %s", i, SDL_GetAudioDeviceName(i, 1));
    }

    if (listAudioDevicesOnly) {
        SDL_Quit();
        return 0;
    }

    timeBeginPeriod(1);

    // ── Print configuration ───────────────────────────────────────
    LOG_INFO("Configuration:");
    LOG_INFO("  url:            %s", config.rtspUrl);
    LOG_INFO("  screen:         %d%s", config.screenIndex,
             config.screenIndex == 0 ? " (all monitors)" : "");
    LOG_INFO("  capture size:   %dx%d",
             config.captureWidth, config.captureHeight);
    if (config.screenIndex > 0) {
        LOG_INFO("  capture offset: (%d, %d)",
                 config.captureOffsetX, config.captureOffsetY);
    }
    LOG_INFO("  output size:    %dx%d", config.outputWidth, config.outputHeight);
    LOG_INFO("  fps:            %d", config.captureFps);
    LOG_INFO("  bitrate/max/buf:%d/%d/%d", config.videoBitrate, config.videoMaxrate, config.videoBufsize);
    LOG_INFO("  crf:            %d", config.crf);
    LOG_INFO("  gop size:       %d", config.gopSize);
    LOG_INFO("  audio:          %s", config.enableAudio ? "enabled" : "disabled");
    if (config.enableAudio) {
        if (config.audioDeviceIndex >= 0) {
            LOG_INFO("  audio device:   index %d", config.audioDeviceIndex);
        } else {
            LOG_INFO("  audio device:   %s", config.audioDeviceName ? config.audioDeviceName : "(default)");
        }
        LOG_INFO("  require audio:  %s", config.requireAudio ? "yes" : "no");
    }
    LOG_INFO("  transport:      %s", config.rtspTransport);
    LOG_INFO("  hw-encoder:     %s (requested)", config.hwEncoder);
    LOG_INFO("  capture method: %s", config.captureMethod);
    LOG_INFO("  log path:       %s", logPath);
    if (statsCsv) {
        LOG_INFO("  stats csv:      %s", statsCsv);
    }
    if (durationSec > 0) {
        LOG_INFO("  duration:       %ds (auto-exit)", durationSec);
    }

    // ── Create pusher ─────────────────────────────────────────────
    RTSPusher pusher;
    PusherStats::setGlobalInstance(pusher.stats());
    pusher.setStateCallback([](PusherState state) {
        const char* names[] = {"Stopped","Opening","Streaming","Recovering","Reconnecting","Error","Closing"};
        LOG_INFO("State: %s", names[(int)state]);
    });
    pusher.setErrorCallback([](const char* msg) {
        LOG_ERROR("Error: %s", msg);
    });

    bool running = true;
    auto requestExit = [&](const char* reason) {
        if (!running) return;
        LOG_INFO("Exit requested: %s", reason);
        running = false;
    };

    LOG_INFO("Opening pusher...");
    if (!pusher.open(config)) {
        requestExit("open failed");
    }

    // Stats CSV
    FILE* statsFile = nullptr;
    if (statsCsv) {
        fopen_s(&statsFile, statsCsv, "w");
        if (statsFile) {
            pusher.stats()->writeCsvHeader(statsFile);
            LOG_INFO("Stats CSV opened: %s", statsCsv);
        }
    }

    // Stats timer: fires every 5 seconds
    auto statsCb = [](Uint32 interval, void* param) -> Uint32 {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_USEREVENT;
        event.user.code = EVENT_STATS;
        event.user.data1 = param;
        SDL_PushEvent(&event);
        return interval;
    };
    SDL_TimerID statsTimerId = 0;
    if (running) {
        statsTimerId = SDL_AddTimer(5000, statsCb, &pusher);
    }

    // Duration timer: one-shot exit after N seconds
    SDL_TimerID durationTimerId = 0;
    if (running && durationSec > 0) {
        auto durationCb = [](Uint32 interval, void* param) -> Uint32 {
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_USEREVENT;
            event.user.code = EVENT_DURATION;
            event.user.data1 = param;
            SDL_PushEvent(&event);
            return 0; // one-shot
        };
        durationTimerId = SDL_AddTimer(durationSec * 1000u, durationCb, nullptr);
        LOG_INFO("Duration timer set: %ds", durationSec);
    }

    while (running) {
        SDL_Event event;
        while (running && SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                requestExit("SDL_QUIT");
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q)
                    requestExit("keyboard");
                break;
            case SDL_USEREVENT:
                switch (event.user.code) {
                case EVENT_RECONNECT:
                    PusherLifecycleManager::handleReconnectEvent(event.user.data1);
                    break;
                case EVENT_DURATION:
                    requestExit("duration");
                    break;
                case EVENT_STATS:
                    if (event.user.data1) {
                        auto* p = static_cast<RTSPusher*>(event.user.data1);
                        PusherStats* s = p->stats();
                        int64_t vMs = s->videoPtsMs.load();
                        int64_t aMs = s->audioPtsMs.load();
                        if (vMs > 0 && aMs > 0) {
                            s->avOffsetMs = aMs - vMs;
                        }
                        if (statsFile) {
                            s->writeCsvRow(statsFile);
                        }
                        // Periodic stats window log
                        LOG_INFO("statsWindowSec=5"
                                 " bitrateKbps=%d"
                                 " videoPackets=%lld audioPackets=%lld"
                                 " windowEncodeMaxUs=%lld windowMuxWriteMaxUs=%lld"
                                 " rawQueueMax=%d encQueueMax=%d"
                                 " rawQueue=%d encQueue=%d"
                                 " vbvUnderflowCount=%lld"
                                 " audioRingBytes=%d audioOverflow=%lld audioUnderrun=%lld"
                                 " videoPtsMs=%lld audioPtsMs=%lld avOffsetMs=%lld"
                                 " ptsErrorCount=%lld"
                                 " firstVideoCapUs=%lld firstAudioCapUs=%lld",
                                 s->bitrateKbps.load(),
                                 (long long)s->videoPacketCount.load(),
                                 (long long)s->audioPacketCount.load(),
                                 (long long)s->windowEncodeMaxUs.load(),
                                 (long long)s->windowMuxWriteMaxUs.load(),
                                 s->windowRawQueueMax.load(),
                                 s->windowEncQueueMax.load(),
                                 s->videoRawQueueDepth.load(),
                                 s->encodedQueueDepth.load(),
                                 (long long)s->vbvUnderflowCount.load(),
                                 s->audioRingBytes.load(),
                                 (long long)s->audioOverflowCount.load(),
                                 (long long)s->audioUnderrunCount.load(),
                                 (long long)vMs,
                                 (long long)aMs,
                                 (long long)s->avOffsetMs.load(),
                                 (long long)s->ptsErrorCount.load(),
                                 (long long)s->firstVideoCaptureUs.load(),
                                 (long long)s->firstAudioCaptureUs.load());
                    }
                    break;
                }
                break;
            }
        }

        // Check stdin for exit command (for headless/process-manager control)
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) {  // 'q', 'Q', or ESC
                requestExit("stdin shutdown");
            }
        }

        // ~1ms tick
        SDL_Delay(1);
    }

    if (statsTimerId) {
        SDL_RemoveTimer(statsTimerId);
    }
    if (durationTimerId) {
        SDL_RemoveTimer(durationTimerId);
    }

    pusher.close();

    if (statsFile) {
        fclose(statsFile);
        statsFile = nullptr;
    }

    timeEndPeriod(1);
    SDL_Quit();
    return 0;
}
