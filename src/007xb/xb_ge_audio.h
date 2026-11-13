#ifndef XB_GE_AUDIO_H
#define XB_GE_AUDIO_H
/* DirectSound sink for the original music.c/libaudio/mixer.c producer.
 * Signed 16-bit stereo at 22050 Hz. Initialization and destruction happen
 * outside the engine's worker lifetime. Submission and presentation-clock
 * sampling are serialized by the sink's critical section. */
typedef unsigned int GEAudioU32;
int GEAudio_Init(void);
void GEAudio_Shutdown(void);
void GEAudio_Tick(void);
int GEAudio_SubmitPCM(const short *stereo,GEAudioU32 frames);
GEAudioU32 GEAudio_QueuedFrames(void);
GEAudioU32 GEAudio_Underruns(void);
GEAudioU32 GEAudio_Overruns(void);
#ifdef __cplusplus
extern "C" {
#endif
int GEAudio_IsReady(void);
#ifdef __cplusplus
}
#endif
#endif
