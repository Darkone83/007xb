#include "xb_ge_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

GEDram::GEDram(): raw_(0),ram_(0) {}

bool GEDram::Init(){
    Shutdown();
    raw_=(unsigned char*)malloc((size_t)GE_N64_DRAM_BYTES+63u);
    if(!raw_)return false;
    /* Original game stores 32-bit pointers and expects aligned allocations. */
    const uintptr_t v=(uintptr_t)raw_;
    ram_=(unsigned char*)((v+63u)&~(uintptr_t)63u);
    memset(ram_,0,GE_N64_DRAM_BYTES);
    return true;
}
void GEDram::Shutdown(){if(raw_)free(raw_);raw_=ram_=0;}
static bool GEOffset(GEU32 address, GEU32 bytes, GEU32* out){
    GEU32 offset=address;
    if(address>=0x70000000u && address<0x70800000u)
        offset=address-0x70000000u;
    else if(address>=0x80000000u && address<0x80800000u)
        offset=address-0x80000000u;
    else if(address>=0xA0000000u && address<0xA0800000u)
        offset=address-0xA0000000u;
    else if(address>=GE_N64_DRAM_BYTES) return false;
    if(offset>GE_N64_DRAM_BYTES || bytes>GE_N64_DRAM_BYTES-offset)
        return false;
    *out=offset;
    return true;
}
void* GEDram::Resolve(GEU32 address,GEU32 bytes){
    GEU32 offset=0;
    return ram_&&GEOffset(address,bytes,&offset)?ram_+offset:0;
}
const void* GEDram::ResolveConst(GEU32 address,GEU32 bytes) const{
    GEU32 offset=0;
    return ram_&&GEOffset(address,bytes,&offset)?ram_+offset:0;
}
void* GEDram::FromPhysical(GEU32 offset,GEU32 bytes){return Resolve(offset,bytes);}
bool GEDram::ToPhysical(const void* ptr,GEU32* offset) const{
    if(!ptr||!ram_||!offset)return false;
    const uintptr_t p=(uintptr_t)ptr,b=(uintptr_t)ram_;
    if(p<b || p-b>=GE_N64_DRAM_BYTES)return false;
    *offset=(GEU32)(p-b);
    return true;
}
bool GEDram::ToGameAddress(const void* ptr,GEU32* address) const{
    GEU32 o=0;if(!address||!ToPhysical(ptr,&o))return false;
    *address=0x70000000u+o;return true;
}

GECart::GECart(): file_(0),base_(0),bytes_(0){
    source_[0]=error_[0]=0;
#if defined(XBOX) || defined(_XBOX)
    InitializeCriticalSection(&lock_);
#else
    pthread_mutex_init(&lock_,0);
#endif
}
void GECart::Close(){
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&lock_);
#else
    pthread_mutex_lock(&lock_);
#endif
    if(file_)fclose(file_);
    file_=0;base_=bytes_=0;source_[0]=0;
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&lock_);
#else
    pthread_mutex_unlock(&lock_);
#endif
}
bool GECart::SetError(const char* text){
    strncpy(error_,text?text:"unknown error",sizeof(error_)-1);
    error_[sizeof(error_)-1]=0;
    return false;
}
bool GECart::OpenFile(const char* path,GEU32 base,GEU32 len){
    Close();error_[0]=0;
    if(!path)return SetError("No ROM file path");
    FILE* f=fopen(path,"rb");
    if(!f)return SetError("Cannot open ROM file");
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return SetError("ROM file seek failed");}
    long end=ftell(f);
    if(end<0 || (GEU32)end<base || len>(GEU32)end-base || len<GE_N64_CART_BYTES){
        fclose(f);return SetError("ROM extent too short");
    }
    file_=f;base_=base;bytes_=len;
    strncpy(source_,path,sizeof(source_)-1);source_[sizeof(source_)-1]=0;
    if(!VerifyHeader()){Close();return false;}
    return true;
}
bool GECart::VerifyHeader(){
    unsigned char h[64];
    if(!file_ || fseek(file_,(long)base_,SEEK_SET)!=0 ||
       fread(h,1,sizeof(h),file_)!=sizeof(h))
        return SetError("Cannot read ROM header");
    if(h[0]!=0x80||h[1]!=0x37||h[2]!=0x12||h[3]!=0x40 ||
       memcmp(h+0x20,"GOLDENEYE",9)!=0)
        return SetError("Not GoldenEye z64 ROM (NTSC US expected)");
    return true;
}
bool GECart::OpenLoose(const char* path){
    FILE* f=fopen(path,"rb");
    if(!f)return SetError("Cannot open loose z64");
    if(fseek(f,0,SEEK_END)!=0){fclose(f);return SetError("Loose ROM seek failed");}
    long size=ftell(f);fclose(f);
    if(size<0 || (unsigned long)size>0x7FFFFFFFul)return SetError("Loose ROM invalid size");
    return OpenFile(path,0,(GEU32)size);
}
bool GECart::OpenArchive(const XBArchive& archive,const char* path){
    const XBArchiveEntry* e=archive.Find("rom/ge007.ntsc-final.z64");
    if(!e)return SetError("GoldenEye ROM entry not found in archive");
    return OpenFile(path,e->offset,e->size);
}
bool GECart::OpenDefault(const XBArchive& archive){
    if(archive.IsOpen() && OpenArchive(archive,"D:\\data\\007xb_assets.dat"))return true;
    if(OpenLoose("D:\\data\\ge007.ntsc-final.z64"))return true;
    if(OpenLoose("D:\\data\\extracted\\ge007.ntsc-final.z64"))return true;
    return SetError("No usable GoldenEye ROM in D:\\data");
}
bool GECart::Read(GEU32 address,void* dst,GEU32 size){
    if(!file_||(!dst&&size)||address<GE_N64_CART_BASE)
        return SetError("Invalid ROM DMA request");
    GEU32 offset=address-GE_N64_CART_BASE;
    if(offset>bytes_ || size>bytes_-offset || base_>0x7FFFFFFFu-offset)
        return SetError("ROM DMA exceeds cartridge range");
    if(!size)return true;
#if defined(XBOX) || defined(_XBOX)
    EnterCriticalSection(&lock_);
#else
    pthread_mutex_lock(&lock_);
#endif
    const int seek=fseek(file_,(long)(base_+offset),SEEK_SET);
    const int ok=(seek==0 && fread(dst,1,size,file_)==size);
#if defined(XBOX) || defined(_XBOX)
    LeaveCriticalSection(&lock_);
#else
    pthread_mutex_unlock(&lock_);
#endif
    if(!ok)return SetError("Short ROM DMA read");
    return true;
}
