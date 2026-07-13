#define SDL_MAIN_HANDLED
#include <SDL.h>

extern "C" {
__declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
__declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
}

#include "RTSPlayer.h"
#include "SDLRenderer.h"
#include "PlayerStats.h"
#include "AVClock.h"
#include "StreamLifecycleManager.h"
#include "logger/Logger.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <time.h>

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
}

static Uint32 onStatsTimer(Uint32 interval, void* param) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = EVENT_STATS;
    event.user.data1 = param;
    SDL_PushEvent(&event);
    return interval; // repeating timer
}

static void pushStreamEofEvent() {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = EVENT_STREAM_EOF;
    SDL_PushEvent(&event);
}

static void printHelp(const char* prog) {
    fprintf(stderr,
        "Usage: %s --url <rtsp_url> [options]\n"
        "\n"
        "Required:\n"
        "  --url <rtsp_url>          RTSP stream URL\n"
        "\n"
        "Options:\n"
        "  --help                    Show this help and exit\n"
        "  --log <path>              Log file path (default: rtsp_player.log)\n"
        "  --csv <path>              CSV stats path (default: rtsp_player_stats.csv)\n"
        "  --no-csv                  Disable CSV stats\n"
        "  --fullscreen              Start in fullscreen mode\n"
        "  --transport <tcp|udp>     RTSP transport protocol (default: tcp)\n"
        "  --title <string>          Window title (default: \"RTSP Player\")\n"
        "  --exit-after <seconds>    Auto-exit after N seconds\n"
        "  --no-audio               Disable audio stream processing\n"
        "  --setpts-zero            Low-latency mode (video + audio if available)\n"
        "  --winid <hwnd>            Render into existing window (HWND, hex or decimal)\n"
        "  --hwaccel <auto|dxva2|none> Hardware decode mode (default: auto; x86 auto uses software)\n"
        "\n"
        "  --winid takes priority over --fullscreen; embedded windows cannot toggle fullscreen.\n"
        "\n"
        "Examples:\n"
        "  %s --url rtsp://192.168.1.100:554/stream\n"
        "  %s --url rtsp://... --transport tcp --fullscreen\n"
        "  %s --url rtsp://... --csv my_stats.csv --log my.log --exit-after 60\n"
        "\n"
        "Note: The old positional-argument form (url log) is still accepted but deprecated.\n"
        , prog, prog, prog, prog);
}

int main(int argc, char* argv[]) {
    // ── Parse command-line arguments ──────────────────────────────
    const char* rtspUrl     = nullptr;
    const char* logPath     = "rtsp_player.log";
    const char* winTitle    = "RTSP Player";
    const char* transport   = "tcp";
    bool        fullscreen  = false;
    const char* winIdStr    = nullptr;
    bool        noAudio     = false;
    bool        setptsZero  = false;
    double      exitAfterSec = 0.0;
    const char* csvPath     = "rtsp_player_stats.csv";   // CSV enabled by default
    bool        csvExplicit  = false;   // true if --csv or --no-csv explicitly set
    const char* hwaccel     = "auto";
    bool        deprecatedPos = false;
    int         positional   = 0;

    for (int i = 1; i < argc; ++i) {
        // ── Known flags (no value) ──────────────────────────────
        if (std::strcmp(argv[i], "--help") == 0) {
            printHelp(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
            continue;
        }
        if (std::strcmp(argv[i], "--no-csv") == 0) {
            csvPath = nullptr;
            csvExplicit = true;
            continue;
        }
        if (std::strcmp(argv[i], "--no-audio") == 0) {
            noAudio = true;
            continue;
        }
        if (std::strcmp(argv[i], "--setpts-zero") == 0) {
            setptsZero = true;
            continue;
        }

        // ── Known options (require a value) ─────────────────────
        auto requireValue = [&](const char* opt) -> const char* {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Error: %s requires a value\n\n", opt);
                printHelp(argv[0]);
                std::exit(1);
            }
            return argv[++i];
        };

        if (std::strcmp(argv[i], "--url") == 0) {
            rtspUrl = requireValue("--url");
        } else if (std::strcmp(argv[i], "--log") == 0) {
            logPath = requireValue("--log");
        } else if (std::strcmp(argv[i], "--csv") == 0) {
            // --csv with optional path; if no path follows, use default name
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                csvPath = argv[++i];
            } else {
                csvPath = "rtsp_player_stats.csv";
            }
            csvExplicit = true;
        } else if (std::strcmp(argv[i], "--transport") == 0) {
            const char* v = requireValue("--transport");
            if (std::strcmp(v, "udp") != 0 && std::strcmp(v, "tcp") != 0) {
                fprintf(stderr, "Error: --transport must be 'udp' or 'tcp', got '%s'\n\n", v);
                printHelp(argv[0]);
                std::exit(1);
            }
            transport = v;
        } else if (std::strcmp(argv[i], "--title") == 0) {
            winTitle = requireValue("--title");
        } else if (std::strcmp(argv[i], "--exit-after") == 0) {
            const char* v = requireValue("--exit-after");
            char* end = nullptr;
            exitAfterSec = std::strtod(v, &end);
            if (end == v || *end != '\0' || exitAfterSec <= 0.0) {
                fprintf(stderr, "Error: --exit-after requires a positive number, got '%s'\n\n", v);
                printHelp(argv[0]);
                std::exit(1);
            }
        } else if (std::strcmp(argv[i], "--hwaccel") == 0) {
            const char* v = requireValue("--hwaccel");
            if (std::strcmp(v, "auto") != 0 && std::strcmp(v, "dxva2") != 0 && std::strcmp(v, "none") != 0) {
                fprintf(stderr, "Error: --hwaccel must be 'auto', 'dxva2', or 'none', got '%s'\n\n", v);
                printHelp(argv[0]);
                std::exit(1);
            }
            hwaccel = v;
        } else if (std::strcmp(argv[i], "--winid") == 0) {
            winIdStr = requireValue("--winid");
        } else if (argv[i][0] == '-') {
            // Unknown flag
            fprintf(stderr, "Error: unknown option '%s'\n\n", argv[i]);
            printHelp(argv[0]);
            std::exit(1);
        } else {
            // Positional argument (deprecated)
            if (!deprecatedPos) {
                deprecatedPos = true;
                fprintf(stderr, "WARNING: Positional arguments are deprecated; use --url and --log instead\n");
            }
            switch (positional++) {
            case 0: rtspUrl = argv[i]; break;
            case 1: logPath = argv[i]; break;
            default:
                fprintf(stderr, "Error: too many positional arguments\n\n");
                printHelp(argv[0]);
                std::exit(1);
            }
        }
    }

    // ── winId overrides fullscreen ──────────────────────────────────
    if (winIdStr) {
        fullscreen = false;
    }

    // ── Init logger ───────────────────────────────────────────────
    logger::Logger::instance().initLogFile(logPath);
    atexit([]() { logger::Logger::instance().closeLogFile(); });

    LOG_INFO("RTSP Player v2 (SDL2) start");

    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(ffmpegLogCallback);

    // ── Init SDL ──────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    timeBeginPeriod(1);

    // ── Create renderer ───────────────────────────────────────────
    std::unique_ptr<SDLRenderer> renderer;
    if (winIdStr) {
        renderer = std::make_unique<SDLRenderer>(winIdStr);
    } else {
        renderer = std::make_unique<SDLRenderer>(winTitle, 1920, 1080, fullscreen);
    }

    // ── Build default URL if none provided ────────────────────────
    char defaultUrl[512];
    if (!rtspUrl) {
        time_t now = time(0);
        char format_time[256];
        strftime(format_time, sizeof(format_time), "%Y_%m_%d", localtime(&now));

        const char* baseUrl = "rtsp://192.168.42.116:25544/";
        strcpy(defaultUrl, baseUrl);
        strcat(defaultUrl, format_time);
        rtspUrl = defaultUrl;
        LOG_INFO("No URL provided, using default: %s", rtspUrl);
    }

    // ── Create player ─────────────────────────────────────────────
    RTSPlayer player;
    player.setRenderer(renderer.get());
    player.setTransport(transport);
    player.setStateCallback([](PlayerState state) {
        const char* names[] = {"Stopped","Connecting","Playing","Recovering","Reconnecting","Error","Closing"};
        LOG_INFO("State: %s", names[(int)state]);
    });
    player.setErrorCallback([](const char* msg) {
        LOG_ERROR("Error: %s", msg);
    });
    player.setEndOfStreamCallback([]() {
        pushStreamEofEvent();
    });

    bool running = true;
    auto requestExit = [&](const char* reason) {
        if (!running) return;
        LOG_INFO("Exit requested: %s", reason);
        running = false;
    };

    // ── Print effective configuration ─────────────────────────────
    {
        char exitBuf[32] = "never";
        if (exitAfterSec > 0.0) snprintf(exitBuf, sizeof(exitBuf), "%.1fs", exitAfterSec);
        LOG_INFO("=== Effective Configuration ===");
        LOG_INFO("  URL:        %s", rtspUrl);
        LOG_INFO("  Log:        %s", logPath);
        LOG_INFO("  CSV:        %s", csvPath ? csvPath : "(disabled)");
        LOG_INFO("  Transport:  %s", transport);
        LOG_INFO("  Fullscreen: %s", fullscreen ? "yes" : "no");
        LOG_INFO("  Audio:      %s", noAudio ? "disabled" : "enabled");
        LOG_INFO("  SetptsZero: %s", setptsZero ? "yes" : "no");
        LOG_INFO("  Winid:      %s", winIdStr ? winIdStr : "(none)");
        LOG_INFO("  HWAccel:    %s", hwaccel);
        LOG_INFO("  Exit after: %s", exitBuf);
        LOG_INFO("===============================");
    }
    if (noAudio) {
        player.setAudioEnabled(false);
    }
    if (setptsZero) {
        player.setSetptsZero(true);
    }
    player.setHwAccel(hwaccel);

    if (!player.open(rtspUrl)) {
        requestExit("open failed");
    }

    // Stats CSV timer: writes every 5 seconds via SDL_USEREVENT
    SDL_TimerID statsTimerId = 0;
    if (running && csvPath) {
        if (player.stats()->initCsv(csvPath)) {
            statsTimerId = SDL_AddTimer(5000, onStatsTimer, player.stats());
            LOG_INFO("CSV stats enabled: %s", csvPath);
        } else {
            LOG_WARN("CSV stats disabled: failed to open %s", csvPath);
            csvPath = nullptr;
        }
    }

    int64_t exitDeadlineUs = 0;
    if (exitAfterSec > 0.0) {
        exitDeadlineUs = av_gettime_relative()
            + static_cast<int64_t>(exitAfterSec * AV_TIME_BASE);
        LOG_INFO("Auto exit enabled: %.3f seconds", exitAfterSec);
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
                else if (event.key.keysym.sym == SDLK_f && !renderer->isEmbedded()) {
                    Uint32 flags = SDL_GetWindowFlags(renderer->window());
                    SDL_SetWindowFullscreen(renderer->window(),
                        (flags & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED)
                    renderer->setWindowSize(event.window.data1, event.window.data2);
                break;
            case SDL_USEREVENT:
                switch (event.user.code) {
                case EVENT_RECONNECT:
                    static_cast<StreamLifecycleManager*>(event.user.data1)->doReconnect();
                    break;
                case EVENT_STATS:
                    static_cast<PlayerStats*>(event.user.data1)->writeCsvRow();
                    break;
                case EVENT_STREAM_EOF:
                    requestExit("stream EOF");
                    break;
                }
                break;
            }
        }

        if (exitDeadlineUs > 0 && av_gettime_relative() >= exitDeadlineUs) {
            requestExit("exit-after reached");
            break;
        }

        int64_t beforeUs = av_gettime_relative();
        player.videoRefresh();
        int64_t elapsedUs = av_gettime_relative() - beforeUs;
        int64_t sleepUs = 1000 - elapsedUs;
        if (sleepUs > 100) {
            av_usleep((unsigned)sleepUs);
        }
    }

    if (statsTimerId) {
        SDL_RemoveTimer(statsTimerId);
        statsTimerId = 0;
    }
    if (csvPath) {
        player.stats()->closeCsv();
    }

    player.close();
    timeEndPeriod(1);
    SDL_Quit();
    return 0;
}
