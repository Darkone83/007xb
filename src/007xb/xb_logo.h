#ifndef XB_LOGO_H
#define XB_LOGO_H
#include "xb_archive.h"

struct XBLogoPoint { float x, y, z; };
struct XBLogoSegment { XB_U32 a, b; };

class XBLogo {
public:
    XBLogo();
    // Lifetime is explicit: Close/Clear/Shutdown, no CRT destructor registration.
    void Clear();
    bool LoadLoose(const char* filename);
    bool LoadArchive(XBArchive& archive, const char* entryName);
    int Points() const { return pointsCount_; }
    int Segments() const { return segmentsCount_; }
    const XBLogoPoint* Vertices() const { return points_; }
    const XBLogoSegment* Lines() const { return segments_; }
    const char* Error() const { return error_; }
private:
    XBLogo(const XBLogo&);
    XBLogo& operator=(const XBLogo&);
    bool Decode(const unsigned char* bytes, XB_U32 size);
    bool Fail(const char* message);
    XBLogoPoint* points_;
    XBLogoSegment* segments_;
    int pointsCount_;
    int segmentsCount_;
    char error_[128];
};
#endif
