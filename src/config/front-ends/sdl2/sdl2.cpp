#include "sdl2.h"
#include <SegmentLdr.h>

#include "keycode_map.h"
#include <wind/pcbridge.h>
#include <algorithm>

#include <SDL.h>

using namespace Executor;

namespace
{
SDL_Window *sdlWindow;
/*SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;*/
SDL_Surface *sdlSurface;
}

SDL2VideoDriver::SDL2VideoDriver(Executor::IEventListener *listener, int& argc, char* argv[])
    : VideoDriver(listener)
{
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        throw std::runtime_error("Failed to initialize SDL.");
    }
    wakeEventType_ = SDL_RegisterEvents(2);
}


void SDL2VideoDriver::endEventLoop()
{
    SDL_Event evt;
    evt.type = wakeEventType_ + 1;
    SDL_PushEvent(&evt);
}


void SDL2VideoDriver::requestUpdate()
{
    SDL_Event evt;
    evt.type = wakeEventType_;
    SDL_PushEvent(&evt);
}

void SDL2VideoDriver::onMainThread(std::function<void()> f)
{
    std::unique_lock lk(mutex_);
    todos_.push_back(f);
    requestUpdate();

    done_.wait(lk, [this] { return todos_.empty(); });
}


bool SDL2VideoDriver::isAcceptableMode(int width, int height, int bpp, bool grayscale_p)
{
    return VideoDriver::isAcceptableMode(width, height, bpp, grayscale_p)
        && bpp != 2;
}


bool SDL2VideoDriver::setMode(int width, int height, int bpp, bool grayscale_p)
{
    onMainThread([&] {
        printf("set_mode: %d %d %d", width, height, bpp);

        if(!width || !height)
        {
            width = framebuffer_.width;
            height = framebuffer_.height;
        }
        if(!width || !height)
        {
            width = VDRIVER_DEFAULT_SCREEN_WIDTH;
            height = VDRIVER_DEFAULT_SCREEN_HEIGHT;
        }
        if(!bpp)
            bpp = framebuffer_.bpp;
        if(!bpp)
            bpp = 8;

        framebuffer_ = Framebuffer(width, height, bpp);

        sdlWindow = SDL_CreateWindow("Window",
                                    SDL_WINDOWPOS_UNDEFINED,
                                    SDL_WINDOWPOS_UNDEFINED,
                                    width, height,
                                    0);
        //SDL_WINDOW_FULLSCREEN_DESKTOP);

        /*sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);

        SDL_RenderSetLogicalSize(sdlRenderer, vdriver_width, vdriver_height);
        SDL_SetRenderDrawColor(sdlRenderer, 128, 128, 128, 255);
        SDL_RenderClear(sdlRenderer);
        SDL_RenderPresent(sdlRenderer);*/

        uint32_t pixelFormat;

        switch(bpp)
        {
            case 1:
                pixelFormat = SDL_PIXELFORMAT_INDEX1LSB;
                break;
            case 4:
                pixelFormat = SDL_PIXELFORMAT_INDEX4LSB;
                break;
            case 8:
                pixelFormat = SDL_PIXELFORMAT_INDEX8;
                break;
            case 16:
                pixelFormat = SDL_PIXELFORMAT_RGB555;
                break;
            case 32:
                pixelFormat = SDL_PIXELFORMAT_BGRX8888;
                break;
        }

#if 1
        uint32_t rmask, gmask, bmask, amask;
        int sdlBpp;
        SDL_PixelFormatEnumToMasks(pixelFormat, &sdlBpp, &rmask, &gmask, &bmask, &amask);

        sdlSurface = SDL_CreateRGBSurfaceFrom(
            framebuffer_.data.get(),
            framebuffer_.width, framebuffer_.height,
            sdlBpp,
            framebuffer_.rowBytes,
            rmask, gmask, bmask, amask);
#else
        sdlSurface = SDL_CreateRGBSurfaceWithFormatFrom(
            framebuffer_.data.get(),
            framebuffer_.width, framebuffer_.height,
            framebuffer_.bpp,
            framebuffer_.rowBytes,
            pixelFormat);
#endif
    });
    return true;
}

void SDL2VideoDriver::setColors(int num_colors, const vdriver_color_t *colors)
{
    onMainThread([&] {
        SDL_Color *sdlColors = (SDL_Color *)alloca(sizeof(SDL_Color) * num_colors);
        for(int i = 0; i < num_colors; i++)
        {
            sdlColors[i].a = 255;
            sdlColors[i].r = colors[i].red >> 8;
            sdlColors[i].g = colors[i].green >> 8;
            sdlColors[i].b = colors[i].blue >> 8;
        }

        SDL_SetPaletteColors(sdlSurface->format->palette, sdlColors, 0, num_colors);

        dirtyRects_.add(0, 0, height(), width());
    });
}

void SDL2VideoDriver::runEventLoop()
{
    SDL_Event event;

    for(;;)
    {
        /* pc rootless: injected per-window input arrives via a ring in
         * shared memory (no DOM/SDL path); poll it a few times per frame.
         * The timeout also bounds injected-input latency. */
        int gotEvent = pcRootlessEnabled()
            ? SDL_WaitEventTimeout(&event, 4)
            : (SDL_WaitEvent(&event), 1);
        pcRootlessDrainInput(callbacks_);
        if(!gotEvent)
            event.type = 0;

        switch(event.type)
        {
            case SDL_MOUSEMOTION:
                callbacks_->mouseMoved(event.motion.x, event.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                callbacks_->mouseButtonEvent(event.button.state == SDL_PRESSED, event.button.x, event.button.y);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                bool down_p;
                unsigned char mkvkey;

                init_sdlk_to_mkv();
                down_p = (event.key.state == SDL_PRESSED);

                /*if(ROMlib_use_scan_codes)
                    mkvkey = ibm_virt_to_mac_virt[event.key.keysym.scancode];
                else*/
                {
                    auto p = sdlk_to_mkv.find(event.key.keysym.sym);
                    if(p == sdlk_to_mkv.end())
                        mkvkey = NOTAKEY;
                    else
                        mkvkey = p->second;
                }
                callbacks_->keyboardEvent(down_p, mkvkey);
            }
            break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                //if(!we_lost_clipboard())
                callbacks_->resumeEvent(false);
                //else
                //{
                //    ZeroScrap();
                //    sendresumeevent(true);
                //}
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                callbacks_->suspendEvent();
                break;
            case SDL_QUIT:
                callbacks_->requestQuit();
                break;
            default:
                if(event.type == wakeEventType_ + 1)
                    return;
        }
        
        std::lock_guard lk(mutex_);
        if(!todos_.empty())
        {
            for(const auto& f : todos_)
                f();
            todos_.clear();
            done_.notify_all();
        }
        if(!dirtyRects_.empty())
        {
            auto rects = dirtyRects_.getAndClear();
            for(const auto& r : rects)
            {
                SDL_Rect sdlR;
                sdlR.x = r.left;
                sdlR.y = r.top;
                sdlR.w = r.right - r.left;
                sdlR.h = r.bottom - r.top;

                SDL_BlitSurface(sdlSurface, &sdlR, SDL_GetWindowSurface(sdlWindow), &sdlR);
            }
            
            SDL_UpdateWindowSurface(sdlWindow);
        }
    }
}

/* This is really inefficient.  We should hash the cursors */
void SDL2VideoDriver::setCursor(char *cursor_data,
                               unsigned short cursor_mask[16],
                               int hotspot_x, int hotspot_y)
{
    /* pc rootless: mirror the cursor into the bridge table — the SDL
     * cursor lands on a detached canvas nobody sees. */
    Executor::pcRootlessSetCursor(cursor_data, cursor_mask, hotspot_x, hotspot_y);
    onMainThread([&] {
        SDL_Cursor *old_cursor, *new_cursor;

        old_cursor = SDL_GetCursor();
        new_cursor = SDL_CreateCursor((unsigned char *)cursor_data,
                                    (unsigned char *)cursor_mask,
                                    16, 16, hotspot_x, hotspot_y);
        if(new_cursor != nullptr)
        {
            SDL_SetCursor(new_cursor);
            SDL_FreeCursor(old_cursor);
        }
    });
}

void SDL2VideoDriver::setCursorVisible(bool show_p)
{
    Executor::pcRootlessSetCursorVisible(show_p);
    onMainThread([&] {
        SDL_ShowCursor(show_p);
    });
}

/* pc rootless: called on the emulator thread from doevent. A pending
 * screen-size request rebuilds the framebuffer + SDL surface (on the SDL
 * thread) and returns true so gd_vdriver_mode_changed() re-plumbs the
 * GDevice, ports, and GrayRgn. */
bool SDL2VideoDriver::updateMode()
{
    int w, h;
    if(!Executor::pcRootlessTakeScreenSizeRequest(&w, &h))
        return false;
    if(w == framebuffer_.width && h == framebuffer_.height)
        return false;

    onMainThread([&] {
        int bpp = framebuffer_.bpp ? framebuffer_.bpp : 32;
        framebuffer_ = Executor::Framebuffer(w, h, bpp);
        std::fill(framebuffer_.data.get(),
                  framebuffer_.data.get()
                      + (size_t)framebuffer_.rowBytes * framebuffer_.height,
                  0);

        if(sdlSurface)
            SDL_FreeSurface(sdlSurface);
        uint32_t pixelFormat = SDL_PIXELFORMAT_BGRX8888;
        uint32_t rmask, gmask, bmask, amask;
        int sdlBpp;
        SDL_PixelFormatEnumToMasks(pixelFormat, &sdlBpp, &rmask, &gmask, &bmask, &amask);
        if(sdlWindow)
            SDL_SetWindowSize(sdlWindow, w, h);
        sdlSurface = SDL_CreateRGBSurfaceFrom(
            framebuffer_.data.get(),
            framebuffer_.width, framebuffer_.height,
            sdlBpp,
            framebuffer_.rowBytes,
            rmask, gmask, bmask, amask);
        dirtyRects_.add(0, 0, h, w);
    });
    return true;
}
