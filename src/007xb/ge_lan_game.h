#ifndef GE_LAN_GAME_H
#define GE_LAN_GAME_H

#include "ge_lan.h"
#include "ge_native_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Game-thread adapter. Device polling/network service never mutates these
 * snapshots. Every accepted tick freezes both input formats for all slots. */
/* Zero rejects a pending LAN stage before allocation; caller reloads title. */
int GE_LanGameStageBegin(int stage);
void GE_LanGameStageLoaded(void);
int GE_LanGameBeginTick(void);
void GE_LanGameEndTick(void);
int GE_LanGameActive(void);
int GE_LanGameIsLocalPlayer(int slot);
unsigned GE_LanGamePlayerCount(void);
const GE_LanPlayerInput *GE_LanGameInput(int slot);
const GE_LanPlayerInput *GE_LanGamePreviousInput(int slot);
int GE_LanGameReadXbox(unsigned slot, struct GEGameXboxState *out);
void GE_LanGameClearXboxEdges(unsigned slot);
void GE_LanGameQuit(int reason);

#ifdef __cplusplus
}
#endif
#endif
