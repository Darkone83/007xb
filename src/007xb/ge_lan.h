#ifndef GE_LAN_H
#define GE_LAN_H

/* First playable LAN test: one local controller per console, 2--4 global
 * players, host-sequenced input lockstep at 30 ticks/sec. All peers must run
 * the same executable and game assets. This is not snapshot replication.
 * Public API is C-compatible; none of these structs is sent as raw bytes. */
#define GE_LAN_MAX_PLAYERS 4
#define GE_LAN_MAX_HOSTS 8
#define GE_LAN_NAME_BYTES 24
#define GE_LAN_PORT 27007
#define GE_LAN_PROTOCOL_VERSION 1
#define GE_LAN_BUILD_ID 0x007B2601u
#define GE_LAN_TICK_DELTA 2u

enum GE_LanState {
    GE_LAN_IDLE=0, GE_LAN_BROWSING, GE_LAN_JOINING, GE_LAN_LOBBY,
    GE_LAN_LOADING, GE_LAN_RUNNING, GE_LAN_ENDING, GE_LAN_ENDED,
    GE_LAN_ERROR
};
enum GE_LanRole { GE_LAN_ROLE_NONE=0, GE_LAN_ROLE_HOST, GE_LAN_ROLE_CLIENT };
enum GE_LanEndReason {
    GE_LAN_END_NONE=0, GE_LAN_END_QUIT, GE_LAN_END_TIMEOUT,
    GE_LAN_END_DESYNC, GE_LAN_END_FINISHED, GE_LAN_END_PROTOCOL,
    GE_LAN_END_NETWORK, GE_LAN_END_LOAD_TIMEOUT, GE_LAN_END_CONTROLLER
};

typedef struct GE_LanPlayerOptions {
    int character, handicap, control_style, team;
} GE_LanPlayerOptions;

typedef struct GE_LanMatchConfig {
    unsigned revision, build_id, player_count;
    int stage_id, mp_stage_index, scenario, weapon_set, game_length;
    int aim_option, difficulty;
    unsigned rng_hi, rng_lo, chr_rng_hi, chr_rng_lo;
    GE_LanPlayerOptions player[GE_LAN_MAX_PLAYERS];
} GE_LanMatchConfig;

typedef struct GE_LanPlayerInput {
    unsigned short buttons, pressed;
    signed char stick_x, stick_y;
    unsigned short native_buttons, native_pressed;
    signed char move_x, move_y, look_x, look_y;
    unsigned char connected, native_connected;
} GE_LanPlayerInput;

typedef struct GE_LanTick {
    unsigned tick, delta, player_count;
    GE_LanPlayerInput input[GE_LAN_MAX_PLAYERS];
} GE_LanTick;

typedef struct GE_LanHostInfo {
    char name[GE_LAN_NAME_BYTES];
    char address[24];
    unsigned player_count, port;
} GE_LanHostInfo;

typedef struct GE_LanStatus {
    int state, role, local_slot;
    unsigned player_count, ready_mask, loaded_mask, tick;
    unsigned discovered_count;
    int end_reason, round_active;
    char message[96];
    char local_address[24];
    char peer_name[GE_LAN_MAX_PLAYERS][GE_LAN_NAME_BYTES];
    GE_LanHostInfo hosts[GE_LAN_MAX_HOSTS];
} GE_LanStatus;

#ifdef __cplusplus
extern "C" {
#endif

/* Init once before game threads. Zero port chooses GE_LAN_PORT. A nonzero
 * alternative port and SetDiscoveryAddress support separate local test
 * processes; normal Xbox builds use defaults. Service is nonblocking and
 * must continue while title, loading, gameplay or END acknowledgement runs.
 * Other API calls are serialized against Service by the platform lock. */
int GE_LanInit(unsigned port);
/* Set after archive validation; included in discovery and every packet. */
void GE_LanSetContentFingerprint(unsigned fingerprint);
void GE_LanService(void);
void GE_LanTearDown(void);
void GE_LanSetDiscoveryAddress(const char *ipv4, unsigned port);
int GE_LanHost(const char *name);
int GE_LanBrowse(void);
int GE_LanJoin(unsigned host_index);
void GE_LanGetStatus(GE_LanStatus *out);
int GE_LanGetConfig(GE_LanMatchConfig *out);
int GE_LanPublishHostConfig(const GE_LanMatchConfig *config);
int GE_LanSetPlayerOptions(const GE_LanPlayerOptions *options);
void GE_LanSetReady(int ready);
int GE_LanRequestStart(void);
/* Same ready check; permits the host to resolve Random's concrete stage_id
 * and select both RNG seeds at commit. All other agreed fields must match. */
int GE_LanRequestStartWithConfig(const GE_LanMatchConfig *resolved);

/* One-shot start configuration handoff to the game thread. All consoles
 * apply it BEFORE allocating stage players and setting either RNG seed.
 * MarkLoaded comes after lvlStageLoad, and no tick is admitted until every
 * player has acknowledged the frozen config and marked loading complete. */
int GE_LanPollStartConfiguration(GE_LanMatchConfig *out);
void GE_LanMarkLoaded(void);

/* Publish physical controller 0 (first connected local pad) after releasing
 * the input bridge's lock, ordinarily each host service poll. Press bits are
 * OR-latched until assigned to exactly one future input tick. Do not consume
 * the game's native pressed-edge latch just to obtain this physical copy. */
void GE_LanPublishLocalInput(const GE_LanPlayerInput *input);

/* Game-thread gate. 1 = copied immutable all-player input and fixed delta;
 * 0 = keep pumping, do not advance game time/input/RNG; -1 = end requested.
 * Call EndTick exactly once after all-player simulation/DL construction.
 * Checksum must cover canonical gameplay state, never pointers/render data. */
int GE_LanTryBeginTick(GE_LanTick *out);
void GE_LanEndTick(unsigned checksum);
void GE_LanRequestEnd(int reason);

/* End is a whole-match terminal state on ANY peer quit, timeout or desync.
 * The host retries END until acknowledgements or a bounded deadline. Peers
 * keep retrying quit requests, so a dropped initial datagram cannot hang.
 * Clear only AFTER the engine has left the match/title reload completed;
 * it disconnects the test session and restores ordinary local input. */
void GE_LanClearAfterEnd(void);
int GE_LanIsMatch(void);
int GE_LanGetLocalSlot(void);
const char *GE_LanEndReasonText(int reason);

#ifdef __cplusplus
}
#endif
#endif
