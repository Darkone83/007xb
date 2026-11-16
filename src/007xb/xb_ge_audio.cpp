/* Native AI output boundary. All notes, envelopes, ADPCM decoding and
 * resampling come from original libaudio plus port/src/mixer.c. */
#include <xtl.h>
#include <dsound.h>
#include <string.h>
#include "xb_ge_audio.h"
#include "ge_native_audio.h"
/* Phase 2: Avoid a redundant hardware cursor/zero-fill pass if the engine's
 * own PCM submit/AI feedback has sampled the cursor during this millisecond.
 * This affects only the periodic host service, never a producer submission
 * or an AI length/queue read. DirectSoundDoWork still runs on every tick.
 * 0 = original service path for immediate A/B/rollback. */
#ifndef GE_XBOX_COALESCE_AUDIO_SERVICE_CURSOR
#define GE_XBOX_COALESCE_AUDIO_SERVICE_CURSOR 1
#endif
extern "C" void mixerInit(void);

#define GE_AUDIO_RATE 22050u
#define GE_AUDIO_RING 65536u
#define GE_AUDIO_ALIGN 4u
#define GE_AUDIO_CUSHION 4096u
#define GE_AUDIO_MAX_QUEUE (2880u*4u)
#define GE_AUDIO_MAX_SUBMIT 4096u

#if defined(_MSC_VER)
typedef unsigned __int64 AudioPosition;
#else
typedef unsigned long long AudioPosition;
#endif
static LPDIRECTSOUND s_device=0;
static LPDIRECTSOUNDBUFFER s_buffer=0;
static CRITICAL_SECTION s_guard;
static int s_guard_ready=0,s_running=0;
static DWORD s_last_cursor=0,s_last_poll_ms=0;
static AudioPosition s_play=0,s_write=0,s_submitted=0;
static unsigned s_last_buffer=0,s_underruns=0,s_overruns=0;

static int Fill(unsigned offset,unsigned bytes,const unsigned char *pcm)
{
    void *a=0,*b=0;DWORD na=0,nb=0;
    if(!bytes)return 1;
    if(!s_buffer || bytes>GE_AUDIO_RING)return 0;
    if(FAILED(s_buffer->Lock(offset,bytes,&a,&na,&b,&nb,0)))return 0;
    if(a&&na){if(pcm)memcpy(a,pcm,na);else memset(a,0,na);}
    if(b&&nb){if(pcm)memcpy(b,pcm+na,nb);else memset(b,0,nb);}
    s_buffer->Unlock(a,na,b,nb);
    return 1;
}
/* Called with the guard held. Clear consumed PCM so an empty looping ring
 * contains silence. The watchdog also handles a complete missed revolution
 * (for example a paused debugger), which a modulo cursor alone cannot see. */
static void Clock()
{
    DWORD play=0,hardware_write=0,now=GetTickCount();
    unsigned delta;
    if(!s_buffer||FAILED(s_buffer->GetCurrentPosition(&play,&hardware_write)))return;
    play&=GE_AUDIO_RING-1u;
    delta=(play+GE_AUDIO_RING-s_last_cursor)&(GE_AUDIO_RING-1u);
    if((DWORD)(now-s_last_poll_ms)>=GE_AUDIO_RING*1000u/(GE_AUDIO_RATE*GE_AUDIO_ALIGN)){
        Fill(0,GE_AUDIO_RING,0);
        s_play+=GE_AUDIO_RING+delta;
        s_write=s_play+GE_AUDIO_CUSHION;
        ++s_underruns;
    }else{
        Fill(s_last_cursor,delta,0);
        s_play+=delta;
        if(s_play>=s_write){
            Fill(0,GE_AUDIO_RING,0);
            s_write=s_play+GE_AUDIO_CUSHION;
            ++s_underruns;
        }
    }
    s_last_cursor=play;s_last_poll_ms=now;
}
static void Stop()
{
    s_running=0;
    if(s_buffer){s_buffer->Stop();s_buffer->Release();s_buffer=0;}
    if(s_device){s_device->Release();s_device=0;}
}
int GEAudio_Init(void)
{
    WAVEFORMATEX format;DSBUFFERDESC descriptor;
    if(s_running)return 1;
    if(FAILED(DirectSoundCreate(0,&s_device,0))||!s_device)return 0;
    memset(&format,0,sizeof(format));memset(&descriptor,0,sizeof(descriptor));
    format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;
    format.nSamplesPerSec=GE_AUDIO_RATE;format.wBitsPerSample=16;
    format.nBlockAlign=GE_AUDIO_ALIGN;
    format.nAvgBytesPerSec=GE_AUDIO_RATE*GE_AUDIO_ALIGN;
    descriptor.dwSize=sizeof(descriptor);descriptor.dwFlags=DSBCAPS_CTRLVOLUME;
    descriptor.dwBufferBytes=GE_AUDIO_RING;descriptor.lpwfxFormat=&format;
    if(FAILED(s_device->CreateSoundBuffer(&descriptor,&s_buffer,0))||!s_buffer){Stop();return 0;}
    if(!Fill(0,GE_AUDIO_RING,0)||FAILED(s_buffer->SetCurrentPosition(0))){Stop();return 0;}
    InitializeCriticalSection(&s_guard);s_guard_ready=1;
    s_play=0;s_write=GE_AUDIO_CUSHION;s_submitted=0;
    s_last_cursor=0;s_last_poll_ms=GetTickCount();s_last_buffer=0;
    s_underruns=0;s_overruns=0;
    mixerInit();
    if(FAILED(s_buffer->Play(0,0,DSBPLAY_LOOPING))){GEAudio_Shutdown();return 0;}
    s_running=1;return 1;
}
void GEAudio_Shutdown(void)
{
    if(s_guard_ready){
        EnterCriticalSection(&s_guard);Stop();LeaveCriticalSection(&s_guard);
        DeleteCriticalSection(&s_guard);s_guard_ready=0;
    }else Stop();
    s_play=s_write=0;s_last_buffer=0;
}
void GEAudio_Tick(void)
{
    if(!s_running||!s_guard_ready)return;
    DirectSoundDoWork();
    EnterCriticalSection(&s_guard);
#if GE_XBOX_COALESCE_AUDIO_SERVICE_CURSOR
    /* s_last_poll_ms is updated only after a successful Clock() cursor read.
     * The guard serializes it with producer/AI reads. Only sub-ms cursor
     * advancement is skipped; the next service pass catches up. PCM
     * producer and AI feedback calls continue to sample fresh. */
    if(GetTickCount()!=s_last_poll_ms)Clock();
#else
    Clock();
#endif
    LeaveCriticalSection(&s_guard);
}
int GEAudio_SubmitPCM(const short *pcm,GEAudioU32 frames)
{
    unsigned bytes;int ok;
    if(!s_running||!s_guard_ready||!pcm||!frames||frames>GE_AUDIO_MAX_SUBMIT)return 0;
    bytes=frames*GE_AUDIO_ALIGN;
    EnterCriticalSection(&s_guard);Clock();
    if(s_write-s_play+bytes>GE_AUDIO_MAX_QUEUE){
        ++s_overruns;LeaveCriticalSection(&s_guard);return 0;
    }
    ok=Fill((unsigned)(s_write&(GE_AUDIO_RING-1u)),bytes,(const unsigned char *)pcm);
    if(ok){s_write+=bytes;s_submitted+=bytes;s_last_buffer=bytes;}
    else ++s_overruns;
    LeaveCriticalSection(&s_guard);return ok;
}
GEAudioU32 GEAudio_QueuedFrames(void)
{
    unsigned bytes;
    if(!s_running||!s_guard_ready)return 0;
    EnterCriticalSection(&s_guard);Clock();
    bytes=(unsigned)(s_write-s_play);
    LeaveCriticalSection(&s_guard);
    return bytes/GE_AUDIO_ALIGN;
}
GEAudioU32 GEAudio_Underruns(void){return s_underruns;}
GEAudioU32 GEAudio_Overruns(void){return s_overruns;}
int GEAudio_IsReady(void){return s_running;}

/* Original port audio.h C ABI. The cushion and one-block bound match the
 * upstream AI feedback contract without changing src/audi.c's regulator. */
int audioInit(void){return GEAudio_Init()?0:-1;}
void audioDestroy(void){GEAudio_Shutdown();}
int32_t audioGetSamplesBuffered(void){return (int32_t)GEAudio_QueuedFrames();}
uint32_t audioGetAiLengthBytes(void)
{
    unsigned bytes;
    if(!s_running||!s_guard_ready)return 0;
    EnterCriticalSection(&s_guard);Clock();
    bytes=(unsigned)(s_write-s_play);
    bytes=bytes>GE_AUDIO_CUSHION?bytes-GE_AUDIO_CUSHION:0;
    if(s_last_buffer&&bytes>s_last_buffer)bytes=s_last_buffer;
    LeaveCriticalSection(&s_guard);return bytes;
}
void audioSetNextBuffer(const int16_t *pcm,uint32_t bytes)
{
    if(bytes&3u)return;
    (void)GEAudio_SubmitPCM(pcm,bytes/GE_AUDIO_ALIGN);
}
int audioDeviceRefresh(void){return s_running?1:0;}
const char *audioDeviceName(int index){return index==0?"Xbox DirectSound":0;}
const char *audioDeviceCurrentName(void){return "Xbox DirectSound";}
void audioDeviceRequest(const char *name){(void)name;}
void audioNotifyDeviceRemoved(uint32_t device){(void)device;}
extern "C" void ge_xbox_audio_service(void){GEAudio_Tick();}
/* Original diagnostic call site in snd.c associates events with PCM bytes.
 * Count real accepted PCM even when no optional capture file is configured. */
extern "C" unsigned long long audioDumpBytePos(void)
{
    AudioPosition bytes;
    if(!s_running||!s_guard_ready)return 0;
    EnterCriticalSection(&s_guard);bytes=s_submitted;LeaveCriticalSection(&s_guard);
    return bytes;
}
