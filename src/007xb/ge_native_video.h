#ifndef GE_NATIVE_VIDEO_H
#define GE_NATIVE_VIDEO_H
#ifdef __cplusplus
extern "C" {
#endif
int GENativeVideo_Bind(void *video,void *renderer,void *logo);
int GENativeVideo_Start(void);
/* Startup diagnostics retire only after the marked legal frame is presented. */
void GENativeVideo_BootStep(const char *message);
void GENativeVideo_BootFrameReady(const void *legalDlEnd);
/* Call after the previous stage's task replies have drained. Detailed traces
 * cover the first three tasks of each load, including same-stage reloads. */
void GENativeVideo_TraceStage(int stage);
int GENativeVideo_Failed(void);
int GENativeVideo_Rendering(void);
unsigned GENativeVideo_Frames(void);
/* Display shape may differ from pixel width/height (anamorphic SD output). */
float videoGetDisplayAspectRatio(void);
void GENativeVideo_SetDiagnostics(int show);
void GENativeVideo_OnFatal(void);
int geNativeRunGraphics(void *commands,unsigned bytes);
int geNativeTaskGraphics(void *task);
int geNativeTaskAudio(void *task);
#ifdef __cplusplus
}
#endif
#endif
