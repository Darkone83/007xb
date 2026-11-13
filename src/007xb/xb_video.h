#ifndef XB_VIDEO_H
#define XB_VIDEO_H
#include <xtl.h>

class XBVideo {
public:
    XBVideo();
    // Lifetime is explicit: Close/Clear/Shutdown, no CRT destructor registration.
    bool Init();
    void Shutdown();
    IDirect3DDevice8* Device() const { return device_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int RefreshRate() const { return refresh_; }
    bool IsWidescreen() const { return widescreen_; }
    bool HasDepth() const { return depth_; }
    const char* ModeName() const { return mode_; }
    void Begin();
    void End();
private:
    IDirect3D8* d3d_;
    IDirect3DDevice8* device_;
    bool depth_;
    int width_, height_;
    int refresh_;
    bool widescreen_;
    const char* mode_;
};
#endif
