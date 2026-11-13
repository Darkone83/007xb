#ifndef GE_NATIVE_BUFFER_H
#define GE_NATIVE_BUFFER_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
/* Scoped scratch bytes with checked allocation; never pulls an STL runtime
 * or throws a C++ exception into the legacy RXDK executable. */
class GENativeBuffer {
    unsigned char* bytes_;
    size_t size_;
    GENativeBuffer(const GENativeBuffer&);
    GENativeBuffer& operator=(const GENativeBuffer&);
public:
    GENativeBuffer():bytes_(0),size_(0) {}
    ~GENativeBuffer() { if(bytes_) free(bytes_); }
    void resize(size_t n) {
        if(n>4u*1024u*1024u) sysFatalError("Texture conversion scratch exceeds budget");
        if(n>size_) {
            void* p=realloc(bytes_,n);
            if(!p && n) sysFatalError("Cannot allocate texture conversion scratch");
            bytes_=(unsigned char*)p;size_=n;
        }
    }
    void assign(const unsigned char* first,const unsigned char* last) {
        if(last<first) sysFatalError("Invalid texture conversion range");
        const size_t n=(size_t)(last-first);resize(n);
        if(n) memcpy(bytes_,first,n);
    }
    unsigned char* data() { return bytes_; }
    unsigned char& operator[](size_t n) { return bytes_[n]; }
};
#endif
