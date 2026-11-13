#ifndef GE_NATIVE_AUDIO_H
#define GE_NATIVE_AUDIO_H

/* Clean host/engine C boundary. Keep N64 PR headers and their CRT shims
 * out of Xbox host translation units. These fixed-width types match the
 * original port's audio.h without importing its engine include tree. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int audioInit(void);
void audioDestroy(void);
int32_t audioGetSamplesBuffered(void);
uint32_t audioGetAiLengthBytes(void);
void audioSetNextBuffer(const int16_t *pcm,uint32_t bytes);

int audioDeviceRefresh(void);
const char *audioDeviceName(int index);
const char *audioDeviceCurrentName(void);
void audioDeviceRequest(const char *name);
void audioNotifyDeviceRemoved(uint32_t device);

int GEAudio_IsReady(void);
void ge_xbox_audio_service(void);
unsigned long long audioDumpBytePos(void);

#ifdef __cplusplus
}
#endif
#endif
