#ifndef XB_ARCHIVE_H
#define XB_ARCHIVE_H

#include <stdio.h>

typedef unsigned int XB_U32;

// Matches 007xb Asset Studio's little-endian 007XBDAT archive v1.
// Independent of XDK/RXDK: usable in host-side tests and on the Xbox.
struct XBArchiveEntry {
    char name[64];
    XB_U32 offset;
    XB_U32 size;
    XB_U32 crc;
    XB_U32 flags;
};

class XBArchive {
public:
    XBArchive();
    // Lifetime is explicit: Close/Clear/Shutdown, no CRT destructor registration.
    bool Open(const char* filename);
    void Close();
    const XBArchiveEntry* Find(const char* name) const;
    bool Read(const XBArchiveEntry* entry, XB_U32 offset, void* dst, XB_U32 bytes);
    bool Verify(const XBArchiveEntry* entry);
    int Count() const { return count_; }
    const char* Error() const { return error_; }
    bool IsOpen() const { return file_ != 0; }
    // Stable manifest fingerprint for LAN compatibility; no disk reads.
    XB_U32 ContentFingerprint() const;
private:
    XBArchive(const XBArchive&);
    XBArchive& operator=(const XBArchive&);
    bool Fail(const char* message);
    bool Seek(XB_U32 offset);
    FILE* file_;
    XBArchiveEntry entries_[64];
    int count_;
    XB_U32 fileBytes_;
    char error_[128];
};

XB_U32 XBCRC32(XB_U32 accumulator, const unsigned char* p, XB_U32 len);
#endif
