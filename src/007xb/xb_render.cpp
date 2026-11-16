#include "xb_render.h"
#include "xb_celmesh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define XB_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define XB_CEL_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)
struct XBCelVertex { float x, y, z; DWORD diffuse; };

#define MAX_LINES 16384
#define CHAR_VERT_CAP 4096
#define MAX_LOGO_POINTS 8192

static void State(IDirect3DDevice8* d)
{
    d->SetTexture(0, 0);
    d->SetPixelShader(0);
    d->SetVertexShader(XB_FVF);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}
static XBScreenVertex SVertex(float x, float y, DWORD col)
{
    XBScreenVertex v = {x, y, 0.5f, 1.0f, col};
    return v;
}
XBRenderer::XBRenderer() : lines_(0), chars_(0), projected_(0), celBuildFailed_(false) {}

bool XBRenderer::Init()
{
    Shutdown();
    celBuildFailed_ = false;
    lines_ = (XBScreenVertex*)malloc(MAX_LINES * 2 * sizeof(XBScreenVertex));
    chars_ = (XBScreenVertex*)malloc(CHAR_VERT_CAP * sizeof(XBScreenVertex));
    projected_ = (float*)malloc(MAX_LOGO_POINTS * 2u * sizeof(float));
    if (!lines_ || !chars_ || !projected_) { Shutdown(); return false; }
    return true;
}
void XBRenderer::Shutdown()
{
    celMesh_.Reset();
    if (lines_) free(lines_);
    if (chars_) free(chars_);
    if (projected_) free(projected_);
    lines_ = chars_ = 0;
    projected_ = 0;
}
void XBRenderer::DrawLogo(IDirect3DDevice8* d, const XBLogo& logo,
                          int width, int height, float radians, float alpha, float zoom)
{
    const int n = logo.Segments();
    const int nv = logo.Points();
    if (!d || !lines_ || !projected_ || n <= 0 || n > MAX_LINES ||
        nv < 2 || nv > MAX_LOGO_POINTS) return;

    // Reproject each unique point ONCE (2424 on this model), not twice for
    // every edge (6774). No D3DX transforms or GPU vertex shader needed.
    const float cy = cosf(radians), sy = sinf(radians);
    // Fixed 0.12 radian tilt: never call sinf/cosf for this each frame.
    const float cx = 0.99280864f, sx = 0.11971221f;
    const float size = ((float)width * 0.65f * zoom) / 10.0f;
    const float middleX = (float)width * 0.5f;
    const float middleY = (float)height * 0.42f;
    const XBLogoPoint* vertices = logo.Vertices();
    const XBLogoSegment* segments = logo.Lines();

    for (int i = 0; i < nv; ++i) {
        const XBLogoPoint& p = vertices[i];
        const float xr = p.x * cy + p.z * sy;
        const float zr = -p.x * sy + p.z * cy;
        const float yr = p.y * cx - zr * sx;
        const float zz = p.y * sx + zr * cx;
        const float perspective = 15.0f / (15.0f + zz);
        projected_[i * 2] = middleX + xr * size * perspective;
        projected_[i * 2 + 1] = middleY - yr * size * perspective;
    }
    for (int i = 0; i < n; ++i) {
        const XBLogoSegment& e = segments[i];
        const DWORD colour = (i % 4) == 0 ? 0xFFFF4FD8u : 0xFFC43CFFu;
        lines_[i * 2] = SVertex(projected_[e.a * 2], projected_[e.a * 2 + 1], colour);
        lines_[i * 2 + 1] = SVertex(projected_[e.b * 2], projected_[e.b * 2 + 1], colour);
    }
    State(d);
    if (alpha < 0.999f) {
        DWORD a = (DWORD)(alpha * 255.0f);
        if (a > 255u) a = 255u;
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_TEXTUREFACTOR, a << 24);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    }
    d->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)n, lines_, sizeof(XBScreenVertex));
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// Little-endian signed 16-bit coordinate decoder, not compiler-specific packing.
static short CelSigned16(const unsigned char* b)
{
    const unsigned short u = (unsigned short)((unsigned)b[0] | ((unsigned)b[1] << 8));
    return (short)u;
}

bool XBRenderer::CreateCelVB(IDirect3DDevice8* d)
{
    if (celMesh_.Ready()) return true;
    if (celBuildFailed_ || !d) return false;
    // 7,170 triangles x 3 x 16 bytes ~336 KiB. Built once on the
    // graphics thread, then transformed entirely by the Xbox NV2A.
    void* mapped = 0;
    if (!celMesh_.BeginBuild(d, XB_CEL_TRIANGLES * 3u,
                             sizeof(XBCelVertex), XB_CEL_FVF, &mapped)) {
        celBuildFailed_ = true;
        return false;
    }
    XBCelVertex* out = (XBCelVertex*)mapped;
    // All geometry work occurs exactly once at boot. No runtime SVG/OBJ
    // triangulation, no per-frame dynamic vertex allocation or CPU transforms.
    const unsigned char* b = XB_CEL_TRI_BYTES;
    for (unsigned t = 0; t < XB_CEL_TRIANGLES; ++t) {
        const DWORD tint = XB_CEL_PALETTE[b[18]];
        for (int v = 0; v < 3; ++v) {
            out->x = (float)CelSigned16(b + 6*v    ) * XB_CEL_SCALE;
            out->y = (float)CelSigned16(b + 6*v + 2) * XB_CEL_SCALE;
            out->z = (float)CelSigned16(b + 6*v + 4) * XB_CEL_SCALE;
            out->diffuse = tint;
            ++out;
        }
        b += 19;
    }
    if (!celMesh_.EndBuild()) { celBuildFailed_ = true; return false; }
    return true;
}

void XBRenderer::DrawCelLogo(IDirect3DDevice8* d, const XBLogo& logo,
                            int width, int height, float radians, float alpha,
                            bool depthAvailable, float zoom)
{
    if (!d || !logo.Points() || alpha <= 0.001f) return;
    // Preserve the known-good wireframe renderer if depth support was rejected.
    // Without Z testing an extruded wordmark is visually incorrect.
    if (!depthAvailable) {
        DrawLogo(d, logo, width, height, radians, alpha, zoom);
        return;
    }
    if (!CreateCelVB(d)) {
        DrawLogo(d, logo, width, height, radians, alpha, zoom);
        return;
    }
    // Xbox fixed-function transform path. Two trig calls for the entire
    // model. GPU handles all 21,510 vertices without CPU-side reprojection.
    const float cy = cosf(radians), sy = sinf(radians);
    const float cx = 0.99280864f, sx = 0.11971221f;
    D3DMATRIX world, view, proj;
    memset(&world, 0, sizeof(world));
    memset(&view, 0, sizeof(view));
    memset(&proj, 0, sizeof(proj));
    world._11 = cy; world._12 = sy*sx; world._13 = sy*cx;
    world._22 = cx; world._23 = -sx;
    world._31 = sy; world._32 = -cy*sx; world._33 = -cy*cx;
    world._11 *= zoom; world._12 *= zoom; world._13 *= zoom;
    world._22 *= zoom; world._23 *= zoom;
    world._31 *= zoom; world._32 *= zoom; world._33 *= zoom;
    world._43 = 12.0f; world._44 = 1.0f;
    view._11 = view._22 = view._33 = view._44 = 1.0f;
    const float aspect = (float)width / (float)height;
    proj._11 = 2.2f / aspect;
    proj._22 = 2.2f;
    proj._33 = 40.0f / 39.0f;
    proj._34 = 1.0f;
    proj._43 = -40.0f / 39.0f;
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetTexture(0, 0);
    d->SetVertexShader(XB_CEL_FVF);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, depthAvailable ? TRUE : FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, depthAvailable ? TRUE : FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    const DWORD a = alpha >= 1.0f ? 255u : (DWORD)(alpha * 255.0f);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, a << 24);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha < 0.999f ? TRUE : FALSE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    celMesh_.DrawTriangles(d);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
}

// Self-contained 5x7 glyphs for diagnostics, not GoldenEye's original font.
// Layout is row-major, bit 4 = leftmost pixel. Original handcrafted glyph bitmaps.
struct Glyph { char c; unsigned char row[7]; };
static const Glyph GLYPHS[] = {
 {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
 {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
 {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
 {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
 {'I',{31,4,4,4,4,4,31}}, {'J',{7,2,2,2,18,18,12}},
 {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
 {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
 {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
 {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
 {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
 {':',{0,4,4,0,4,4,0}}, {'-',{0,0,0,31,0,0,0}},
 {'.',{0,0,0,0,0,12,12}}, {'/',{1,2,2,4,8,8,16}},
 {'+',{0,4,4,31,4,4,0}}, {'(' ,{2,4,8,8,8,4,2}},
 {')',{8,4,2,2,2,4,8}}, {'_',{0,0,0,0,0,0,31}},
 {' ',{0,0,0,0,0,0,0}}, {'?',{14,17,1,2,4,0,4}}
};
static const unsigned char* GetGlyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (unsigned i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); ++i)
        if (GLYPHS[i].c == c) return GLYPHS[i].row;
    return GLYPHS[sizeof(GLYPHS)/sizeof(GLYPHS[0])-1].row;
}
static void Quad(XBScreenVertex* out, int& pos, float x, float y, float w, DWORD col)
{
    out[pos++] = SVertex(x, y, col);
    out[pos++] = SVertex(x+w, y, col);
    out[pos++] = SVertex(x, y+w, col);
    out[pos++] = SVertex(x+w, y, col);
    out[pos++] = SVertex(x+w, y+w, col);
    out[pos++] = SVertex(x, y+w, col);
}
void XBRenderer::DrawText(IDirect3DDevice8* d, float x, float y,
                          float scale, const char* message, DWORD colour)
{
    if (!d || !chars_ || !message) return;
    int count = 0;
    float base = x;
    State(d);
    for (const char* cp = message; *cp; ++cp) {
        if (*cp == '\n') { x = base; y += 9 * scale; continue; }
        const unsigned char* rows = GetGlyph(*cp);
        for (int row = 0; row < 7; ++row) for (int bit = 0; bit < 5; ++bit) {
            if (!(rows[row] & (1u << (4-bit)))) continue;
            if (count + 6 > CHAR_VERT_CAP) {
                d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(count / 3), chars_, sizeof(XBScreenVertex));
                count = 0;
            }
            Quad(chars_, count, x + bit * scale, y + row * scale, scale, colour);
        }
        x += 6 * scale;
    }
    if (count) d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(count / 3), chars_, sizeof(XBScreenVertex));
}
