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

private:
    void requestUpdate() override;
    void onMainThread(std::function<void()> f); 

    uint32_t wakeEventType_;
    std::vector<std::function<void()>> todos_;
    std::condition_variable done_;
};