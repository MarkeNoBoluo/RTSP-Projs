#include "SDLRenderer.h"
#include "Common.h"
#include <cstdlib>
#include <cstring>
#include "logger/Logger.h"

#include <SDL.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

SDLRenderer::SDLRenderer(const char* title, int w, int h, bool fullscreen)
    : m_title(title)
    , m_winW(w)
    , m_winH(h)
{
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN;
    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    m_window = SDL_CreateWindow(title,
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                w, h,
                                flags);
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }

    m_renderer = SDL_CreateRenderer(m_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer) {
        LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
        return;
    }

    // 启动后立即清黑一帧，避免白屏
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderPresent(m_renderer);

    SDL_RendererInfo rinfo;
    if (SDL_GetRendererInfo(m_renderer, &rinfo) == 0) {
        LOG_INFO("SDL renderer: %s (flags=0x%x max=%dx%d)", rinfo.name, rinfo.flags,
                 rinfo.max_texture_width, rinfo.max_texture_height);
    }

    LOG_INFO("SDL renderer created: %dx%d fullscreen=%d", w, h, fullscreen);
}

SDLRenderer::SDLRenderer(const char* winIdStr)
    : m_title("RTSP Player (embedded)")
    , m_embedded(true)
{
#ifdef _WIN32
    char* end = nullptr;
    uintptr_t hwnd = std::strtoull(winIdStr, &end, 0);
    if (end == winIdStr || *end != '\0') {
        LOG_ERROR("Invalid --winId value: '%s'", winIdStr);
        return;
    }

    m_window = SDL_CreateWindowFrom(reinterpret_cast<void*>(hwnd));
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindowFrom failed: %s", SDL_GetError());
        return;
    }

    SDL_GetWindowSize(m_window, &m_winW, &m_winH);

    m_renderer = SDL_CreateRenderer(m_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer) {
        LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
        return;
    }
#else
    LOG_WARN("--winId is not supported on Linux, ignoring");
    (void)winIdStr;
    return;
#endif

    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderPresent(m_renderer);

    SDL_RendererInfo rinfo;
    if (SDL_GetRendererInfo(m_renderer, &rinfo) == 0) {
        LOG_INFO("SDL renderer: %s (flags=0x%x max=%dx%d)", rinfo.name, rinfo.flags,
                 rinfo.max_texture_width, rinfo.max_texture_height);
    }

    LOG_INFO("SDL renderer embedded: winId=%s size=%dx%d", winIdStr, m_winW, m_winH);
}

SDLRenderer::~SDLRenderer() {
    destroy();
}

bool SDLRenderer::init(int width, int height) {
    recreateTexture(width, height);
    updateDisplayRect();
    return m_texture != nullptr;
}

bool SDLRenderer::ensureSwsContext(int width, int height, int srcFormat) {
    if (m_swsBufW != width || m_swsBufH != height) {
        delete[] m_swsBuf;
        m_swsBuf = nullptr;
        m_swsBufW = 0;
        m_swsBufH = 0;
        if (m_swsCtx) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }
        m_swsSrcFormat = -1;
    }
    if (!m_swsBuf) {
        int ySize  = width * height;
        int uvSize = width * height / 2;
        m_swsBuf   = new uint8_t[ySize + uvSize + 64];
        m_swsBufW  = width;
        m_swsBufH  = height;
    }

    if (m_swsCtx && m_swsSrcFormat == srcFormat) {
        return true;
    }

    sws_freeContext(m_swsCtx);
    m_swsCtx = sws_getContext(width, height, static_cast<AVPixelFormat>(srcFormat),
                              width, height, AV_PIX_FMT_NV12,
                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        const char* srcName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(srcFormat));
        LOG_ERROR("sws_getContext failed for %dx%d %s->NV12",
                  width, height, srcName ? srcName : "unknown");
        return false;
    }
    m_swsSrcFormat = srcFormat;
    return true;
}

int SDLRenderer::convertToNV12(AVFrame* src, uint8_t** outPlanes, int* outStrides) {
    int ySize  = m_swsBufW * m_swsBufH;
    int width  = m_swsBufW;

    outPlanes[0] = m_swsBuf;
    outPlanes[1] = m_swsBuf + ySize;
    outPlanes[2] = nullptr;
    outPlanes[3] = nullptr;
    outStrides[0] = width;
    outStrides[1] = width;
    outStrides[2] = 0;
    outStrides[3] = 0;

    int ret = sws_scale(m_swsCtx,
                        src->data, src->linesize, 0, src->height,
                        outPlanes, outStrides);
    return ret;
}

void SDLRenderer::displayFrame(AVFrame* frame) {
    if (!m_renderer || !frame || !frame->data[0]) return;

    if (frame->width != m_texW || frame->height != m_texH || !m_texture) {
        recreateTexture(frame->width, frame->height);
        updateDisplayRect();
    }
    if (!m_texture) return;

    Uint64 t0 = SDL_GetPerformanceCounter();

    uint8_t* updateY = nullptr;
    uint8_t* updateUV = nullptr;
    int updateYStride = 0;
    int updateUVStride = 0;
    bool converted = false;

    if (frame->format == AV_PIX_FMT_NV12) {
        if (!frame->data[1]) return;
        updateY = frame->data[0];
        updateUV = frame->data[1];
        updateYStride = frame->linesize[0];
        updateUVStride = frame->linesize[1];
    } else {
        if (!ensureSwsContext(frame->width, frame->height, frame->format)) return;

        uint8_t* nv12Planes[4];
        int      nv12Strides[4];
        int scaledHeight = convertToNV12(frame, nv12Planes, nv12Strides);
        if (scaledHeight <= 0) return;

        updateY = nv12Planes[0];
        updateUV = nv12Planes[1];
        updateYStride = nv12Strides[0];
        updateUVStride = nv12Strides[1];
        converted = true;
    }

    Uint64 t1 = SDL_GetPerformanceCounter();

#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_UpdateNVTexture(m_texture, nullptr,
                        updateY, updateYStride,
                        updateUV, updateUVStride);
#else
    // SDL < 2.0.16: use LockTexture / UnlockTexture
    {
        void* pixels = nullptr;
        int pitch = 0;
        if (SDL_LockTexture(m_texture, nullptr, &pixels, &pitch) == 0) {
            uint8_t* dst = static_cast<uint8_t*>(pixels);
            // Copy Y plane
            for (int row = 0; row < m_texH; row++) {
                memcpy(dst + row * pitch,
                       updateY + row * updateYStride, m_texW);
            }
            // Copy UV plane (interleaved, half height)
            uint8_t* uvDst = dst + pitch * m_texH;
            int uvHeight = m_texH / 2;
            int uvWidth = m_texW;
            for (int row = 0; row < uvHeight; row++) {
                memcpy(uvDst + row * pitch,
                       updateUV + row * updateUVStride, uvWidth);
            }
            SDL_UnlockTexture(m_texture);
        }
    }
#endif

    Uint64 t2 = SDL_GetPerformanceCounter();

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, &m_dstRect);
    SDL_RenderPresent(m_renderer);

    Uint64 t3 = SDL_GetPerformanceCounter();

    static int frameCount = 0;
    frameCount++;
    if (frameCount <= 5 || frameCount % 30 == 0) {
        Uint64 freq = SDL_GetPerformanceFrequency();
        double swsMs   = converted ? 1000.0 * (t1 - t0) / freq : 0.0;
        double updMs   = 1000.0 * (t2 - t1) / freq;
        double rendMs  = 1000.0 * (t3 - t2) / freq;
        double totalMs = 1000.0 * (t3 - t0) / freq;
        LOG_INFO("Render #%d: sws=%.1fms update=%.1fms rc+present=%.1fms total=%.1fms",
                 frameCount, swsMs, updMs, rendMs, totalMs);
    }
}

void SDLRenderer::setWindowSize(int w, int h) {
    m_winW = w;
    m_winH = h;
    updateDisplayRect();
}

void SDLRenderer::destroy() {
    if (m_swsCtx)  { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    delete[] m_swsBuf;  m_swsBuf = nullptr;
    m_swsBufW = 0; m_swsBufH = 0;
    m_swsSrcFormat = -1;

    if (m_texture)  { SDL_DestroyTexture(m_texture);   m_texture  = nullptr; }
    if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);     m_window   = nullptr; }
}

void SDLRenderer::recreateTexture(int width, int height) {
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }

    m_texture = SDL_CreateTexture(m_renderer,
                                  SDL_PIXELFORMAT_NV12,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  width, height);
    if (!m_texture) {
        LOG_ERROR("SDL_CreateTexture failed: %s", SDL_GetError());
        return;
    }

    m_texW = width;
    m_texH = height;
    LOG_INFO("Texture created: %dx%d", width, height);
}

void SDLRenderer::updateDisplayRect() {
    if (m_texW <= 0 || m_texH <= 0 || m_winW <= 0 || m_winH <= 0) {
        m_dstRect = {0, 0, m_winW, m_winH};
        return;
    }

    double vidAspect = (double)m_texW / m_texH;
    double winAspect = (double)m_winW / m_winH;

    if (vidAspect > winAspect) {
        m_dstRect.w = m_winW;
        m_dstRect.h = (int)(m_winW / vidAspect);
        m_dstRect.x = 0;
        m_dstRect.y = (m_winH - m_dstRect.h) / 2;
    } else {
        m_dstRect.h = m_winH;
        m_dstRect.w = (int)(m_winH * vidAspect);
        m_dstRect.x = (m_winW - m_dstRect.w) / 2;
        m_dstRect.y = 0;
    }
}
