#pragma once

#include <vdriver/vdriver.h>
#include <vector>
#include <functional>
#include <condition_variable>

class SDL2VideoDriver : public Executor::VideoDriver
{
public:
    SDL2VideoDriver(Executor::IEventListener *listener, int& argc, char* argv[]);

    bool isAcceptableMode(int width, int height, int bpp, bool grayscale_p) override;
    bool setMode(int width, int height, int bpp, bool grayscale_p) override;
    void setColors(int num_colors, const Executor::vdriver_color_t *colors) override;
    void setCursor(char *cursor_data, uint16_t cursor_mask[16], int hotspot_x, int hotspot_y) override;
    void setCursorVisible(bool show_p) override;

    void runEventLoop() override;
    void endEventLoop() override;

    /* pc rootless: consume a pending screen-size request on the emulator
     * thread (doevent polls this; true → gd_vdriver_mode_changed runs). */
    bool updateMode() override;

    /* pc clipboard bridge (issue #659): the TEXT scrap flavor ↔ the host
     * (pc / browser) clipboard, so copy/paste crosses per-instance engines
     * and reaches the rest of pc. Non-TEXT flavors fall through to the base
     * in-memory Scrap. */
    void putScrap(Executor::OSType type, Executor::LONGINT length,
                  char *p, int scrap_count) override;
    Executor::LONGINT getScrap(Executor::OSType type,
                               Executor::Handle h) override;

private:
    void requestUpdate() override;
    void onMainThread(std::function<void()> f); 

    uint32_t wakeEventType_;
    std::vector<std::function<void()>> todos_;
    std::condition_variable done_;
};