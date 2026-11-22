/* Native Xbox LAN game-thread integration. All player simulation and display
 * list construction runs in the original order on every peer. The display
 * list itself carries local-view ownership; no render-thread global is used. */
#include <ultra64.h>
#include <string.h>
#include "ge_lan_game.h"
#include "ge_native_platform.h"
#include "boss.h"
#include "random.h"
#include "game/player.h"
#include "game/bondview.h"
#include "game/frametiming.h"
#include "game/front.h"
#include "game/lv.h"
#include "game/mpmenu.h"
#include "game/cheat.h"
#include "game/options.h"
#include "game/chrai.h"

#if defined(GE_XBOX_NATIVE32)
extern u64 g_chrObjRandomSeed;
extern s32 D_80048394;
extern s32 D_800483A8;
extern void updateFrameCounters(s32 deltaFrames);
extern void frontLanReturnToMenu(void);

static int s_active;
static int s_loaded;
static int s_tickOpen;
static int s_endHandled;
static int s_localSlot;
static int s_stage;
static unsigned s_count;
static unsigned s_nativeEdgesRead;
static GE_LanTick s_tick;
static GE_LanTick s_previousTick;
static GE_LanPlayerInput s_neutral;
static s32 s_savedAppendSolo, s_savedAppendMulti;
static unsigned s_savedOptions[PLAYER_OPTION_RATIO + 1];

static void geLanCommonOptions(void)
{
    /* Protocol v1 transmits per-player control style and the MP aim rule.
     * Other solo/watch preferences must not alter another peer's simulation.
     * Upright look, autoaim capability on (MP rule still gates it), hold aim,
     * sight/look-ahead/ammo on, full-screen, 16:9. Restore after the round. */
    static const unsigned values[PLAYER_OPTION_RATIO + 1] = {1,1,0,1,1,1,0,1};
    unsigned i;
    for (i = 0; i <= PLAYER_OPTION_RATIO; ++i)
        game_options_entries[i].current_value = values[i];
}

int GE_LanGameActive(void) { return s_active; }
int GE_LanGameIsLocalPlayer(int slot) { return !s_active || slot == s_localSlot; }
unsigned GE_LanGamePlayerCount(void) { return s_active ? s_count : 0; }

const GE_LanPlayerInput *GE_LanGameInput(int slot)
{
    if (!s_active) return NULL;
    if (slot < 0 || (unsigned)slot >= s_count) return &s_neutral;
    return &s_tick.input[slot];
}

const GE_LanPlayerInput *GE_LanGamePreviousInput(int slot)
{
    if (!s_active) return NULL;
    if (slot < 0 || (unsigned)slot >= s_count) return &s_neutral;
    return &s_previousTick.input[slot];
}

int GE_LanGameReadXbox(unsigned slot, struct GEGameXboxState *out)
{
    const GE_LanPlayerInput *in = GE_LanGameInput((int)slot);
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!in) return 0;
    out->buttons = in->native_buttons;
    if (slot < 4 && !(s_nativeEdgesRead & (1u << slot))) {
        out->pressed = in->native_pressed;
        s_nativeEdgesRead |= 1u << slot;
    }
    out->move_x = in->move_x; out->move_y = in->move_y;
    out->look_x = in->look_x; out->look_y = in->look_y;
    out->connected = in->native_connected;
    return out->connected;
}

void GE_LanGameClearXboxEdges(unsigned slot)
{
    if (slot < 4) s_nativeEdgesRead |= 1u << slot;
}

int GE_LanGameStageBegin(int stage)
{
    GE_LanMatchConfig config;
    GE_LanStatus status;
    unsigned i;
    if (stage == LEVELID_TITLE) {
        if (s_active) {
            s_active = 0;
            s_tickOpen = 0;
            memset(&s_tick, 0, sizeof(s_tick));
            g_AppendCheatSinglePlayer = s_savedAppendSolo;
            g_AppendCheatMultiPlayer = s_savedAppendMulti;
            for (i = 0; i <= PLAYER_OPTION_RATIO; ++i)
                game_options_entries[i].current_value = s_savedOptions[i];
            frontLanReturnToMenu();
        }
        return 1;
    }
    if (!GE_LanIsMatch()) return 1;
    /* Own the transition before validation, so a failed/aborted START can
     * never fall through to ordinary local simulation or leave LAN input on. */
    s_active = 1;
    s_loaded = s_tickOpen = s_endHandled = 0;
    s_count = 0;
    s_localSlot = GE_LanGetLocalSlot();
    s_nativeEdgesRead = 0;
    memset(&s_tick, 0, sizeof(s_tick));
    memset(&s_neutral, 0, sizeof(s_neutral));
    memset(&s_previousTick, 0, sizeof(s_previousTick));
    s_savedAppendSolo = g_AppendCheatSinglePlayer;
    s_savedAppendMulti = g_AppendCheatMultiPlayer;
    for (i = 0; i <= PLAYER_OPTION_RATIO; ++i)
        s_savedOptions[i] = game_options_entries[i].current_value;
    GE_LanGetStatus(&status);
    if (status.state == GE_LAN_ENDING || status.state == GE_LAN_ENDED ||
            status.state == GE_LAN_ERROR) return 0;
    if (!GE_LanGetConfig(&config) || config.player_count < 2 ||
            config.player_count > 4 || config.stage_id != stage ||
            s_localSlot < 0 || (unsigned)s_localSlot >= config.player_count) {
        GE_LanRequestEnd(GE_LAN_END_PROTOCOL);
        return 0;
    }
    s_count = config.player_count;
    s_stage = stage;
    /* The same full 64-bit states must precede player allocation, setup
     * loading, spawn selection and every RNG-consuming load operation. */
    g_randomSeed = ((u64)config.rng_hi << 32) | config.rng_lo;
    g_chrObjRandomSeed = ((u64)config.chr_rng_hi << 32) | config.chr_rng_lo;
    lastFrameCounter = previousFrameCounter = -1;
    currentFrameCounter = halfFrameCounter = isFrameCounterOdd = 0;
    halfMinusPreviousCounter = 0;
    speedgraphframes = 0;
    frameDelay = 1;
    D_80048394 = D_800483A8 = 0;
    copy_of_osgetcount_value_0 = copy_of_osgetcount_value_1 = osGetCount();
    /* LAN v1 has no cheat-settings protocol. Keep the local preferences,
     * disable their application for this round, and restore on title. */
    geLanCommonOptions();
    g_AppendCheatSinglePlayer = g_AppendCheatMultiPlayer = 0;
    memset(g_CheatPlayerTextRelated, 0, sizeof(g_CheatPlayerTextRelated));
    GENativePlatform_Trace("LAN START stage=%d slot=%d players=%u\n",
        s_stage, s_localSlot, s_count);
    return 1;
}

void GE_LanGameStageLoaded(void)
{
    if (!s_active) return;
    /* init_watch_at_start_of_stage may apply a local save preference. */
    geLanCommonOptions();
    s_loaded = 1;
    g_ClockTimer = speedgraphframes = 0;
    g_GlobalTimerDelta = 0.0f;
    GE_LanMarkLoaded();
}

static void geLanGameEndStage(void)
{
    GE_LanStatus status;
    if (s_endHandled) return;
    s_endHandled = 1;
    GE_LanGetStatus(&status);
    if (status.end_reason == GE_LAN_END_QUIT ||
            status.end_reason == GE_LAN_END_FINISHED)
        GENativePlatform_Trace("LAN END stage=%d slot=%d players=%u tick=%u reason=%d\n",
            s_stage, s_localSlot, s_count, status.tick, status.end_reason);
    else
        GENativePlatform_TraceIssue("LAN END stage=%d slot=%d players=%u tick=%u reason=%d\n",
            s_stage, s_localSlot, s_count, status.tick, status.end_reason);
    s_tickOpen = 0;
    memset(&s_tick, 0, sizeof(s_tick));
    /* A missing peer must never strand the MENU_FINISHED all-player quorum.
     * Existing awards/controls cleanup runs once, then the ordinary stage
     * drain/unload returns to the LAN menu. No missing input is simulated. */
    if (s_loaded && getPlayerCount() >= 2 && !g_gameOverFlag)
        mpCalculateAwards(FALSE);
    lvlSetControlsLockedFlag(1);
    g_ClockTimer = speedgraphframes = 0;
    bossSetLoadedStage(LEVELID_TITLE);
}

int GE_LanGameBeginTick(void)
{
    GE_LanTick next;
    int result;
    if (!s_active) return 1;
    result = GE_LanTryBeginTick(&next);
    if (result < 0) { geLanGameEndStage(); return -1; }
    if (!result) return 0;
    if (!s_loaded || next.delta != GE_LAN_TICK_DELTA ||
            next.player_count != s_count || s_tickOpen) {
        GE_LanRequestEnd(GE_LAN_END_PROTOCOL);
        geLanGameEndStage();
        return -1;
    }
    s_previousTick = s_tick;
    s_tick = next;
    s_nativeEdgesRead = 0;
    s_tickOpen = 1;
    updateFrameCounters((s32)next.delta);
    frameDelay = 1;
    return 1;
}

static unsigned geLanHashWord(unsigned h, unsigned value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) {
        h = (h ^ (value & 255u)) * 16777619u;
        value >>= 8;
    }
    return h;
}
static unsigned geLanHashFloat(unsigned h, f32 value)
{
    unsigned word;
    memcpy(&word, &value, sizeof(word));
    return geLanHashWord(h, word);
}
static unsigned geLanHashPosition(unsigned h, const coord3d *position)
{
    h = geLanHashFloat(h, position->x);
    h = geLanHashFloat(h, position->y);
    return geLanHashFloat(h, position->z);
}
static unsigned geLanGameChecksum(void)
{
    unsigned h = 2166136261u;
    unsigned i, j, props = 0;
    PropRecord *prop;
    h = geLanHashWord(h, s_tick.tick);
    h = geLanHashWord(h, (unsigned)g_randomSeed);
    h = geLanHashWord(h, (unsigned)(g_randomSeed >> 32));
    h = geLanHashWord(h, (unsigned)g_chrObjRandomSeed);
    h = geLanHashWord(h, (unsigned)(g_chrObjRandomSeed >> 32));
    h = geLanHashWord(h, (unsigned)currentFrameCounter);
    h = geLanHashWord(h, (unsigned)g_GlobalTimer);
    h = geLanHashWord(h, (unsigned)g_ClockTimer);
    h = geLanHashWord(h, (unsigned)D_80048394);
    h = geLanHashWord(h, (unsigned)g_stopPlayFlag);
    h = geLanHashWord(h, (unsigned)g_gameOverFlag);
    for (i = 0; i < s_count; ++i) {
        const struct player *p = g_playerPointers[i];
        h = geLanHashWord(h, i);
        h = geLanHashWord(h, p != NULL);
        if (!p) continue;
        if (p->prop) h = geLanHashPosition(h, &p->prop->pos);
        h = geLanHashFloat(h, p->vv_theta);
        h = geLanHashFloat(h, p->vv_verta);
        h = geLanHashFloat(h, p->bondhealth);
        h = geLanHashFloat(h, p->bondarmour);
        h = geLanHashFloat(h, p->actual_health);
        h = geLanHashFloat(h, p->actual_armor);
        h = geLanHashWord(h, p->bonddead);
        h = geLanHashWord(h, p->crouchpos);
        h = geLanHashWord(h, p->kills_this_life);
        for (j = 0; j < 30; ++j) h = geLanHashWord(h, p->ammoheldarr[j]);
        for (j = 0; j < 2; ++j) {
            h = geLanHashWord(h, p->hands[j].weaponnum);
            h = geLanHashWord(h, p->hands[j].weapon_ammo_in_magazine);
            h = geLanHashWord(h, p->hands[j].weapon_action_state);
        }
        h = geLanHashWord(h, g_playerPlayerData[i].kill_count);
        for (j = 0; j < 4; ++j)
            h = geLanHashWord(h, g_playerPlayerData[i].kill_counts[j]);
        h = geLanHashWord(h, g_playerPlayerData[i].order_out_in_yolt);
        h = geLanHashWord(h, g_playerPlayerData[i].have_token_or_goldengun);
    }
    /* Hash values in the simulation's active list order, never addresses,
     * structure padding, audio handles or visibility/render caches. */
    for (prop = chrpropGetActiveTail(); prop && props < 4096u; prop = prop->prev) {
        ++props;
        h = geLanHashWord(h, prop->type);
        h = geLanHashWord(h, (unsigned)prop->timetoregen);
        h = geLanHashPosition(h, &prop->pos);
        if (prop->type == PROP_TYPE_CHR && prop->chr) {
            h = geLanHashWord(h, (unsigned)prop->chr->chrnum);
            h = geLanHashWord(h, (unsigned)prop->chr->actiontype);
            h = geLanHashFloat(h, prop->chr->damage);
        } else if ((prop->type == PROP_TYPE_OBJ || prop->type == PROP_TYPE_WEAPON ||
                    prop->type == PROP_TYPE_DOOR) && prop->obj) {
            h = geLanHashFloat(h, prop->obj->damage);
            if (prop->type == PROP_TYPE_DOOR) h = geLanHashFloat(h, prop->door->frac);
        }
    }
    return geLanHashWord(h, props);
}

void GE_LanGameEndTick(void)
{
    if (!s_active || !s_tickOpen) return;
    GE_LanEndTick(geLanGameChecksum());
    s_tickOpen = 0;
}

void GE_LanGameQuit(int reason)
{
    if (!s_active) return;
    GE_LanRequestEnd(reason);
    geLanGameEndStage();
}
#endif
