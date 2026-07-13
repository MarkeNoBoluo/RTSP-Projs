#pragma once

#include <cstdint>

struct AVFrame;
struct SwsContext;

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(int width, int height) = 0;
    virtual void displayFrame(AVFrame* frame) = 0;
    virtual void setWindowSize(int w, int h) = 0;
    virtual void destroy() = 0;
};

#include <SDL.h>

class SDLRenderer : public IRenderer {
public:
    SDLRenderer(const char* title, int w, int h, bool fullscreen = false);
    explicit SDLRenderer(const char* winIdStr);
    ~SDLRenderer() override;

    SDLRenderer(const SDLRenderer&) = delete;
    SDLRenderer& operator=(const SDLRenderer&) = delete;
    SDLRenderer(SDLRenderer&&) = delete;
    SDLRenderer& operator=(SDLRenderer&&) = delete;

    bool init(int width, int height) override;
    void displayFrame(AVFrame* frame) override;
    void setWindowSize(int w, int h) override;
    void destroy() override;

    SDL_Window* window() const { return m_window; }
    bool isEmbedded() const { return m_embedded; }

private:
    void recreateTexture(int width, int height);
    void updateDisplayRect();
    bool ensureSwsContext(int width, int height, int srcFormat);
    int convertToNV12(AVFrame* src, uint8_t** outPlanes, int* outStrides);

    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture  = nullptr;
    SDL_Rect      m_dstRect{0, 0, 0, 0};

    SwsContext* m_swsCtx   = nullptr;
    uint8_t*    m_swsBuf   = nullptr;
    int         m_swsBufW  = 0;
    int         m_swsBufH  = 0;
    int         m_swsSrcFormat = -1;

    const char*   m_title;
    bool m_embedded = false;
    int m_texW = 0;
    int m_texH = 0;
    int m_winW = 0;
    int m_winH = 0;
};
