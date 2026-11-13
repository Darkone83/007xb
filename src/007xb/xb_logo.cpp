#include "xb_logo.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static XB_U32 LE32(const unsigned char* p)
{
    return ((XB_U32)p[0]) | ((XB_U32)p[1] << 8) |
           ((XB_U32)p[2] << 16) | ((XB_U32)p[3] << 24);
}
static float LEFloat(const unsigned char* p)
{
    XB_U32 u = LE32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
XBLogo::XBLogo() : points_(0), segments_(0), pointsCount_(0), segmentsCount_(0)
{
    error_[0] = 0;
}

void XBLogo::Clear()
{
    if (points_) free(points_);
    if (segments_) free(segments_);
    points_ = 0;
    segments_ = 0;
    pointsCount_ = 0;
    segmentsCount_ = 0;
}
bool XBLogo::Fail(const char* message)
{
    strncpy(error_, message, sizeof(error_) - 1);
    error_[sizeof(error_) - 1] = 0;
    return false;
}
bool XBLogo::Decode(const unsigned char* b, XB_U32 size)
{
    Clear();
    if (!b || size < 24 || memcmp(b, "D83LINES", 8) != 0 || LE32(b + 8) != 1 || LE32(b + 20) != 0)
        return Fail("Invalid darkone83.lines header");
    XB_U32 nv = LE32(b + 12), nl = LE32(b + 16);
    if (nv < 2 || nv > 8192 || nl == 0 || nl > 16384 || 24u + nv * 12u + nl * 8u != size)
        return Fail("Invalid darkone83.lines counts/size");
    points_ = (XBLogoPoint*)malloc(nv * sizeof(XBLogoPoint));
    segments_ = (XBLogoSegment*)malloc(nl * sizeof(XBLogoSegment));
    if (!points_ || !segments_) { Clear(); return Fail("Not enough memory for logo"); }
    for (XB_U32 i = 0; i < nv; ++i) {
        const unsigned char* p = b + 24u + i * 12u;
        points_[i].x = LEFloat(p);
        points_[i].y = LEFloat(p + 4);
        points_[i].z = LEFloat(p + 8);
        if (!(fabs(points_[i].x) < 100000.0f && fabs(points_[i].y) < 100000.0f &&
              fabs(points_[i].z) < 100000.0f))
            { Clear(); return Fail("Invalid logo vertex coordinates"); }
    }
    const unsigned char* lines = b + 24u + nv * 12u;
    for (XB_U32 i = 0; i < nl; ++i) {
        segments_[i].a = LE32(lines + 8u * i);
        segments_[i].b = LE32(lines + 8u * i + 4u);
        if (segments_[i].a >= nv || segments_[i].b >= nv)
            { Clear(); return Fail("Logo line vertex index out of bounds"); }
    }
    pointsCount_ = (int)nv;
    segmentsCount_ = (int)nl;
    return true;
}
bool XBLogo::LoadArchive(XBArchive& archive, const char* entryName)
{
    error_[0] = 0;
    const XBArchiveEntry* e = archive.Find(entryName);
    if (!e) return Fail("Logo entry absent from 007xb archive");
    if (e->size > 250000u) return Fail("Logo entry too large");
    if (!archive.Verify(e)) return Fail("Logo entry CRC invalid");
    unsigned char* b = (unsigned char*)malloc(e->size);
    if (!b) return Fail("Cannot allocate logo buffer");
    bool readOK = archive.Read(e, 0, b, e->size);
    bool ok = readOK && Decode(b, e->size);
    free(b);
    if (!readOK) return Fail(archive.Error());
    return ok;
}
bool XBLogo::LoadLoose(const char* filename)
{
    error_[0] = 0;
    FILE* f = fopen(filename, "rb");
    if (!f) return Fail("Loose logo not found");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return Fail("Logo size failed"); }
    long n = ftell(f);
    if (n < 24 || n > 250000 || fseek(f, 0, SEEK_SET) != 0)
        { fclose(f); return Fail("Invalid loose logo size"); }
    unsigned char* b = (unsigned char*)malloc(n);
    if (!b) { fclose(f); return Fail("Cannot allocate loose logo"); }
    bool ok = fread(b, 1, n, f) == (size_t)n;
    fclose(f);
    if (ok) ok = Decode(b, (XB_U32)n);
    free(b);
    if (!ok && !error_[0]) return Fail("Loose logo read failed");
    return ok;
}
