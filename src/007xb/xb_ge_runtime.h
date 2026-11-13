#ifndef XB_GE_RUNTIME_H
#define XB_GE_RUNTIME_H
#include "xb_ge_memory.h"
#include "xb_ge_os.h"

/* Owned platform services for the original GoldenEye engine.
 * Prepares DRAM, provides cartridge DMA, starts a genuine retrace timer,
 * and can launch the ORIGINAL mainproc(void*) when that C source is linked.
 * It deliberately has no synthetic menu/title state machine.
 * No game entry is called unless linked code explicitly supplies mainproc.
 */
class GERuntime {
public:
    GERuntime();
    bool Init(const XBArchive& archive,const char* archivePath=0);
    void Shutdown();
    bool Ready() const {return dram_.AllocatedBytes()!=0 && cart_.Ready() && os_.IsRunning();}
    bool MemoryReady() const {return dram_.AllocatedBytes()!=0;}
    bool CartReady() const {return cart_.Ready();}
    bool RetraceReady() const {return os_.IsRunning();}
    bool DMAFromROM(GEU32 cartAddress,GEU32 n64Dest,GEU32 bytes);
    bool RunOriginalMain(void (*mainproc)(void*));
    const char* Error() const {return error_;}
    GEDram& Dram() {return dram_;}
    GECart& Cart() {return cart_;}
    GEOSKernel& OS() {return os_;}
private:
    GERuntime(const GERuntime&);
    GERuntime& operator=(const GERuntime&);
    GEDram dram_;
    GECart cart_;
    GEOSKernel os_;
    GEOSThread gameThread_;
    char error_[128];
    bool gameStarted_;
};
#endif
