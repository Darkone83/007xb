#include "xb_archive.h"
#include <stdlib.h>
#include <string.h>

// IEEE CRC32, compatible with zlib.crc32() and Asset Studio.
// 256-entry lookup replaces eight branch/shift iterations per input byte.
// One-time integer initialization: no C++ global constructor or CRT dependencies.
XB_U32 XBCRC32(XB_U32 accumulator, const unsigned char* p, XB_U32 len)
{
    static XB_U32 table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (XB_U32 n = 0; n < 256; ++n) {
            XB_U32 c = n;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
            table[n] = c;
        }
        tableReady = true;
    }
    XB_U32 crc = ~accumulator;
    for (XB_U32 i = 0; i < len; ++i)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
static XB_U32 ReadLE32(const unsigned char* p)
{
    return ((XB_U32)p[0]) | ((XB_U32)p[1] << 8) |
           ((XB_U32)p[2] << 16) | ((XB_U32)p[3] << 24);
}

XBArchive::XBArchive() : file_(0), count_(0), fileBytes_(0)
{
    error_[0] = '\0';
    memset(entries_, 0, sizeof(entries_));
}

void XBArchive::Close()
{
    if (file_) fclose(file_);
    file_ = 0;
    count_ = 0;
    fileBytes_ = 0;
    memset(entries_, 0, sizeof(entries_));
}
bool XBArchive::Fail(const char* message)
{
    strncpy(error_, message, sizeof(error_) - 1);
    error_[sizeof(error_) - 1] = 0;
    return false;
}
bool XBArchive::Seek(XB_U32 offset)
{
    // Avoid relying on 64-bit seek APIs in classic RXDK's 32-bit CRT.
    if (offset > 0x7FFFFFFFu) return Fail("Archive position exceeds 2GiB");
    if (fseek(file_, (long)offset, SEEK_SET) != 0) return Fail("Archive seek failed");
    return true;
}
bool XBArchive::Open(const char* filename)
{
    Close();
    error_[0] = 0;
    if (!filename || !(file_ = fopen(filename, "rb")))
        return Fail("007xb_assets.dat not found");
    if (fseek(file_, 0, SEEK_END) != 0) { Close(); return Fail("Archive size read failed"); }
    long actual = ftell(file_);
    if (actual < 0 || (unsigned long)actual > 0x7FFFFFFFul) {
        Close(); return Fail("Unsupported archive size");
    }
    fileBytes_ = (XB_U32)actual;
    if (fileBytes_ < 32 || !Seek(0)) { Close(); return Fail("Short archive"); }
    unsigned char h[32];
    if (fread(h, 1, sizeof(h), file_) != sizeof(h)) { Close(); return Fail("Short archive header"); }
    if (memcmp(h, "007XBDAT", 8) != 0 || ReadLE32(h + 8) != 1)
        { Close(); return Fail("Archive magic/version mismatch"); }
    XB_U32 num = ReadLE32(h + 12);
    XB_U32 dirOff = ReadLE32(h + 16);
    XB_U32 recordBytes = ReadLE32(h + 20);
    XB_U32 declaredBytes = ReadLE32(h + 24);
    XB_U32 directoryCRC = ReadLE32(h + 28);
    if (num < 1 || num > 64 || dirOff != 32 || recordBytes != 80 ||
        declaredBytes != fileBytes_ || num * 80u > fileBytes_ - 32u)
        { Close(); return Fail("Archive directory bounds invalid"); }
    unsigned char raw[64 * 80];
    if (!Seek(dirOff) || fread(raw, 80, num, file_) != num)
        { Close(); return Fail("Cannot read archive directory"); }
    if (XBCRC32(0, raw, num * 80u) != directoryCRC)
        { Close(); return Fail("Archive directory CRC mismatch"); }
    XB_U32 minimum = ((32u + 80u * num + 4095u) / 4096u) * 4096u;
    for (XB_U32 i = 0; i < num; ++i) {
        const unsigned char* p = raw + 80u * i;
        if (!memchr(p, 0, 64)) { Close(); return Fail("Unterminated archive entry name"); }
        memcpy(entries_[i].name, p, 64);
        entries_[i].offset = ReadLE32(p + 64);
        entries_[i].size = ReadLE32(p + 68);
        entries_[i].crc = ReadLE32(p + 72);
        entries_[i].flags = ReadLE32(p + 76);
        XBArchiveEntry& a = entries_[i];
        if (!a.name[0] || a.flags != 0 || a.size == 0 ||
            (a.offset & 4095u) || a.offset < minimum || a.size > fileBytes_ ||
            a.offset > fileBytes_ - a.size)
            { Close(); return Fail("Invalid archive entry extent or flags"); }
        for (XB_U32 j = 0; j < i; ++j) {
            const XBArchiveEntry& b = entries_[j];
            if (!strcmp(a.name, b.name)) { Close(); return Fail("Duplicate archive entry"); }
            if (a.offset < b.offset + b.size && b.offset < a.offset + a.size)
                { Close(); return Fail("Overlapping archive entries"); }
        }
    }
    count_ = (int)num;
    return true;
}
const XBArchiveEntry* XBArchive::Find(const char* name) const
{
    if (!file_ || !name) return 0;
    for (int i = 0; i < count_; ++i)
        if (strcmp(entries_[i].name, name) == 0) return &entries_[i];
    return 0;
}
XB_U32 XBArchive::ContentFingerprint() const
{
    if(!file_)return 0u;
    XB_U32 hash=2166136261u;
    // Explicit bytes avoid structure padding/host endianness. Offsets are
    // intentionally excluded: changing file offsets preserves compatibility.
    for(int i=0;i<count_;++i) {
        const XBArchiveEntry& e=entries_[i];
        for(unsigned j=0;j<64u && e.name[j];++j)
            hash=(hash^(unsigned char)e.name[j])*16777619u;
        hash=(hash^0u)*16777619u;
        const XB_U32 values[3]={e.size,e.crc,e.flags};
        for(unsigned k=0;k<3u;++k)
            for(unsigned shift=0;shift<32u;shift+=8u)
                hash=(hash^((values[k]>>shift)&255u))*16777619u;
    }
    return hash ? hash : 1u;
}
bool XBArchive::Read(const XBArchiveEntry* e, XB_U32 offset, void* dst, XB_U32 bytes)
{
    if (!file_ || !e || (!dst && bytes)) return Fail("Archive not open/read arguments invalid");
    if (offset > e->size || bytes > e->size - offset) return Fail("Entry read out of bounds");
    if (!bytes) return true;
    if (!Seek(e->offset + offset)) return false;
    if (fread(dst, 1, bytes, file_) != bytes) return Fail("Short archive entry read");
    return true;
}
bool XBArchive::Verify(const XBArchiveEntry* e)
{
    if (!file_ || !e) return Fail("Entry missing from archive");
    if (!Seek(e->offset)) return false;
    // Single-threaded asset probe; fixed buffer in BSS keeps stack usage low.
    static unsigned char buf[16384];
    XB_U32 remain = e->size;
    XB_U32 crc = 0;
    while (remain) {
        XB_U32 part = remain < sizeof(buf) ? remain : (XB_U32)sizeof(buf);
        if (fread(buf, 1, part, file_) != part) return Fail("Short archive entry during CRC");
        crc = XBCRC32(crc, buf, part);
        remain -= part;
    }
    return crc == e->crc ? true : Fail("Archive entry CRC mismatch");
}
