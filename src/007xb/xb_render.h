#ifndef XB_RENDER_H
#define XB_RENDER_H
#include <xtl.h>
#include "xb_logo.h"
#include "xb_native_mesh.h"

// Pre-transformed 2D vertices: avoids any D3DX runtime dependencies.
struct XBScreenVertex { float x, y, z, rhw; DWORD colour; };

class XBRenderer {
public:
    XBRenderer();
    // Lifetime is explicit: Close/Clear/Shutdown, no CRT destructor registration.
    bool Init();
    void Shutdown();
    // Original wireframe mode is retained as a live A/B reference.
    void DrawLogo(IDirect3DDevice8* d, const XBLogo& logo, int width, int height,
                  float radians, float alpha, float zoom = 1.0f);
    // Packed cel-shaded triangle mesh: built into a single GPU VB once, not
    // transformed/triangulated on the Xbox CPU every frame.
    void DrawCelLogo(IDirect3DDevice8* d, const XBLogo& referenceLogo,
                     int width, int height, float radians, float alpha,
                     bool depthAvailable, float zoom = 1.0f);
    bool HasCelMesh() const { return celMesh_.Ready(); }
    void DrawText(IDirect3DDevice8* d, float x, float y, float scale, const char* message, DWORD colour);
private:
    XBRenderer(const XBRenderer&);
    XBRenderer& operator=(const XBRenderer&);
    XBScreenVertex* lines_;
    XBScreenVertex* chars_;
    // 8192 x/y float pairs; reused while drawing to avoid reprojection per edge.
    float* projected_;
    XBNativeMesh celMesh_;
    bool celBuildFailed_;
    bool CreateCelVB(IDirect3DDevice8* d);
};
#endif
