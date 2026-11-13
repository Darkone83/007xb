#include "xb_ge_runtime.h"
#include "xb_ge_libultra.h"
#if defined(GE_XBOX_NATIVE32)
#include "ge_native_platform.h"
#endif
#include <string.h>
GERuntime::GERuntime():gameStarted_(false){
    memset(&gameThread_,0,sizeof(gameThread_));error_[0]=0;
}
bool GERuntime::Init(const XBArchive& archive,const char* archivePath){
    if(gameStarted_)return false;
    Shutdown();error_[0]=0;
    if(!dram_.Init()){
        strcpy(error_,"Cannot reserve 8 MB N64 DRAM");return false;
    }
    if(!(archivePath?cart_.OpenArchive(archive,archivePath):cart_.OpenDefault(archive))){
        strncpy(error_,cart_.Error(),sizeof(error_)-1);error_[sizeof(error_)-1]=0;
        dram_.Shutdown();return false;
    }
    /* NTSC US ROM: game timing is 60 Hz irrespective of the Xbox video mode. */
    if(!os_.Init(60u)||!os_.StartRetrace()){
        strcpy(error_,"Cannot start N64 VI retrace kernel");
        os_.Shutdown();GEOSUnbindGameRuntime();cart_.Close();dram_.Shutdown();return false;
    }
    GEOSBindGameRuntime(this);
    return true;
}
void GERuntime::Shutdown(){
    /* Never kill a live original-game thread and free memory from under it. */
    if(gameStarted_)return;
#if defined(GE_XBOX_NATIVE32)
    if(!GENativePlatform_TraceShutdown())return;
#endif
    os_.Shutdown();GEOSUnbindGameRuntime();cart_.Close();dram_.Shutdown();
}
bool GERuntime::DMAFromROM(GEU32 cartAddress,GEU32 dest,GEU32 bytes){
    void* target=dram_.Resolve(dest,bytes);
    if(!target)return false;
    return cart_.Read(cartAddress,target,bytes);
}
bool GERuntime::RunOriginalMain(void (*mainproc)(void*)){
    if(!Ready()||!mainproc||gameStarted_)return false;
    if(!os_.Spawn(&gameThread_,mainproc,0,5,10))return false;
    gameStarted_=true;return true;
}
