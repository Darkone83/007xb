/*
 * PRNG — ported verbatim from src/random.s (MIPS assembly, not compiled for
 * the PC). This is a REAL implementation, not a stub: the game uses it for
 * genuine gameplay logic, so a no-op would silently corrupt behaviour.
 *
 *   - randomGetNextFrom() feeds the CRC in src/game/crc.c
 *   - g_randomSeed is persisted in replay state (src/game/ramromreplay.c)
 *   - randomGetNext() drives RANDOMFRAC()/RANDOMGETNEXT_F32() (bondconstants.h)
 *
 * Also ports src/game/chrObjRandom.s: the character-AI PRNG (chroid.c),
 * which runs the identical transform on its own seed variable.
 *
 * The seed is stored as a full u64 and the transform genuinely operates on
 * all 64 bits of the register (see the dsll32/dsrl32 note below) — the
 * stored seed is NOT truncated to 32 bits between calls, matching what the
 * real `sd $a0, g_randomSeed` in random.s stores. The initial seed is the
 * two .words in random.s: 0xAB8D9F77 (hi) / 0x81280783 (lo). Only the low
 * 32 bits are ever returned to callers (`dsll32 $v0,$a0,0; dsra32
 * $v0,$v0,0` sign-extends the low half into $v0, of which callers only use
 * the u32 truncation).
 *
 * D284 (M-140): the original port of this file had dsll32/dsrl32 backwards
 * — it shifted by the literal immediate `n` and masked to 32 bits, i.e. it
 * modelled them as 32-bit shift instructions. Real MIPS64 dsll32/dsrl32 are
 * "shift by n+32", operating on the FULL 64-bit register with no masking:
 * they exist specifically to encode a 32..63 shift amount in a 5-bit field.
 * The bug was bit-exact-but-wrong: every generated value differed from the
 * real N64 stream from the very first draw, silently desyncing loot RNG, AI
 * variance, and replay determinism in every release to date. Verified two
 * independent ways (a forked review agent's numeric trace, and a hand trace
 * of random.s against this file, both in the M-138 review session).
 */

#define GE_NATIVE_RANDOM_IMPLEMENTATION 1
#include <ultra64.h>
#include <stdlib.h>
#include <string.h>
#include <random.h>
#if defined(GE_XBOX_NATIVE32)
#include "ge_native_platform.h"
#endif

u64 g_randomSeed = 0xAB8D9F7781280783ULL;

/*
 * MIPS64 shift helpers, matching the .s instructions exactly:
 *   dsll32 rd,rs,n  ->  (rs << (n+32))   (full 64-bit register, no masking)
 *   dsrl32 rd,rs,n  ->  (rs >> (n+32))   (full 64-bit register, unsigned)
 * Plain `<<`/`>>` on u64 match dsll/dsrl (full 64-bit shifts, shift amount
 * used as-is, 0-31).
 */
static inline u64 dsll32(u64 x, int n) { return x << (n + 32); }
static inline u64 dsrl32(u64 x, int n) { return x >> (n + 32); }

/*
 * Advance the global seed and return the new low 32 bits.
 * Mirrors randomGetNext in random.s line-by-line.
 */
/* D318R: env-gated draw logger (GE_D318R=1). Logs the first N draws with
 * tick + caller return address to identify which call sites consume the
 * gameplay PRNG and whether their per-run counts are stable. Diagnostic
 * only; inert without the env var. */
#ifdef PORT
#include <PR/os.h>
extern s32 g_GlobalTimer;
static void randomDrawLog(void)
{
    static int  s_on  = -1;
    static u32  s_n   = 0;
    const u32   MAXN  = 4000;

    if (s_on < 0)
    {
        const char *e = getenv("GE_D318R");
        s_on = (e && e[0] == '1') ? 1 : 0;
    }
    if (s_on && s_n < MAXN)
    {
        osSyncPrintf("D318R: n=%u t=%d ra=%p\n",
                     s_n, (int)g_GlobalTimer, __builtin_return_address(0));
        s_n++;
    }
}
#endif /* PORT */

#if defined(GE_XBOX_NATIVE32)
static u32 randomAdvanceGlobal(void)
#else
u32 randomGetNext(void)
#endif
{
#ifdef PORT
    randomDrawLog();
#endif
    u64 a0 = g_randomSeed;
    u64 a1, a2;

    a2 = dsll32(a0, 0x1f);   /* dsll32 $a2, $a0, 0x1f */
    a1 = a0 << 0x1f;         /* dsll   $a1, $a0, 0x1f */
    a2 = a2 >> 0x1f;         /* dsrl   $a2, $a2, 0x1f */
    a1 = dsrl32(a1, 0);      /* dsrl32 $a1, $a1, 0    */
    a0 = dsll32(a0, 0xc);    /* dsll32 $a0, $a0, 0xc  */
    a2 = a2 | a1;            /* or     $a2, $a2, $a1  */
    a0 = dsrl32(a0, 0);      /* dsrl32 $a0, $a0, 0    */
    a2 = a2 ^ a0;            /* xor    $a2, $a2, $a0  */
    a0 = a2 >> 0x14;         /* dsrl   $a0, $a2, 0x14 */
    a0 = a0 & 0xfff;         /* andi   $a0, $a0, 0xfff*/
    a0 = a0 ^ a2;            /* xor    $a0, $a0, $a2  */

    g_randomSeed = a0;       /* sd     $a0, g_randomSeed */
    return (u32)a0;          /* v0 = a0 & 0xFFFFFFFF    */
}

#if defined(GE_XBOX_NATIVE32)
/* These fixed buffers are owned by the replay/game thread, like g_randomSeed.
 * No allocations, formatting, file writes or OS calls occur while capturing.
 * A replay mismatch freezes the last calls before writing any issue records.
 * The transform above is unchanged and is called exactly once per request. */
#define RNG_TRACE_CAPACITY 256U
#define RNG_TRACE_CHECKPOINTS 8U
#ifndef GE_XBOX_RNG_FULL_TRACE
#define GE_XBOX_RNG_FULL_TRACE 0
#endif
/* Full caller history remains in RAM, but Release writes only the last
 * twelve calls after an actual mismatch. Full per-call trace is opt-in. */
#define RNG_TRACE_COMPACT_CALLS 12U
#define RNG_TRACE_COMPACT_CHECKS 4U
/* Cumulative per-consumer counts at each packet boundary. Unlike the last
 * 256 call records, these remain exact when a particle/effect burst takes
 * hundreds of draws in one packet. No additional RNG advances are made. */
enum NativeRngConsumer {
    RNG_CONSUMER_GUN, RNG_CONSUMER_GUNFIRE,
    RNG_CONSUMER_CHRACTION, RNG_CONSUMER_CHRAI, RNG_CONSUMER_CHR,
    RNG_CONSUMER_PLAYER, RNG_CONSUMER_PROP, RNG_CONSUMER_EXPLOSION,
    RNG_CONSUMER_GLASS, RNG_CONSUMER_OTHER, RNG_CONSUMER_COUNT
};
struct NativeRngTraceEntry
{
    u64 before;
    u64 after;
    const char *file;
    u32 line;
    u32 sequence;
    u32 packet;
    s32 tick;
};
struct NativeRngTraceCheckpoint
{
    u64 state;
    u32 packet;
    s32 tick;
    u32 draws;
    u32 consumerDraws[RNG_CONSUMER_COUNT];
};
static struct NativeRngTraceEntry s_rngTrace[RNG_TRACE_CAPACITY];
static struct NativeRngTraceCheckpoint s_rngChecks[RNG_TRACE_CHECKPOINTS];
typedef char NativeRngTraceStorageBound[
    sizeof(s_rngTrace) + sizeof(s_rngChecks) <= 12U * 1024U ? 1 : -1];
static u64 s_rngTraceSeed;
static u32 s_rngTraceNext;
static u32 s_rngTraceKept;
static u32 s_rngTraceDraws;
static u32 s_rngTracePacket;
static u32 s_rngCheckNext;
static u32 s_rngCheckKept;
static u32 s_rngConsumerDraws[RNG_CONSUMER_COUNT];
static s32 s_rngTraceActive;
static s32 s_rngTraceDumped;
/* 11 short fixed-width lines for xemu ISO screenshots. They are captured
 * only on the first RNG mismatch, not during ordinary gameplay. Display
 * uses these immutable strings after the game thread sets s_rngScreenReady. */
#define RNG_SCREEN_LINES 11U
#define RNG_SCREEN_WIDTH 112U
static char s_rngScreenText[RNG_SCREEN_LINES][RNG_SCREEN_WIDTH];
static volatile u32 s_rngScreenReady;
static const char *randomTraceBasename(const char *file);
#define RNG_SOURCE_CACHE 16U
static const char *s_rngConsumerFileCache[RNG_SOURCE_CACHE];
static u8 s_rngConsumerGroupCache[RNG_SOURCE_CACHE];
static u32 s_rngConsumerCacheNext;
static u32 randomTraceConsumerFor(const char *file)
{
    u32 i, result = RNG_CONSUMER_OTHER;
    const char *name;
    for (i = 0; i < RNG_SOURCE_CACHE; ++i)
        if (s_rngConsumerFileCache[i] == file && file)
            return s_rngConsumerGroupCache[i];
    name = randomTraceBasename(file);
    if (!strcmp(name, "gun.c")) result = RNG_CONSUMER_GUN;
    else if (!strcmp(name, "gunfire.c")) result = RNG_CONSUMER_GUNFIRE;
    else if (!strcmp(name, "chraction.c")) result = RNG_CONSUMER_CHRACTION;
    else if (!strcmp(name, "chrai.c")) result = RNG_CONSUMER_CHRAI;
    else if (!strcmp(name, "chr.c")) result = RNG_CONSUMER_CHR;
    else if (!strcmp(name, "player.c")) result = RNG_CONSUMER_PLAYER;
    else if (!strcmp(name, "propobj.c")) result = RNG_CONSUMER_PROP;
    else if (!strcmp(name, "explosion.c")) result = RNG_CONSUMER_EXPLOSION;
    else if (!strcmp(name, "glass2.c")) result = RNG_CONSUMER_GLASS;
    if (file) {
        s_rngConsumerFileCache[s_rngConsumerCacheNext] = file;
        s_rngConsumerGroupCache[s_rngConsumerCacheNext] = (u8)result;
        s_rngConsumerCacheNext = (s_rngConsumerCacheNext + 1U) % RNG_SOURCE_CACHE;
    }
    return result;
}

const char *randomReplayTraceScreenLine(u32 index)
{
    if (!s_rngScreenReady || index >= RNG_SCREEN_LINES) return NULL;
    return s_rngScreenText[index];
}

/* Count only draws since the preceding accepted checkpoint. This does not
 * advance either RNG stream, query wall-clock time or write any file. */
void randomReplayTraceFreezeScreen(u32 packet, s32 tick, u8 expected,
                                   u8 actual, u64 state, s32 gunSync100)
{
    u32 prevPacket = 0, beginDraw = 0, endDraw = s_rngTraceDraws;
    u32 gun = 0, ai = 0, prop = 0, other = 0, i, ringStart;
    const struct NativeRngTraceEntry *last[4] = {NULL,NULL,NULL,NULL};
    u32 lastN = 0;
    if (s_rngScreenReady) return;
    if (s_rngCheckKept >= 2U)
    {
        u32 previous = (s_rngCheckNext + RNG_TRACE_CHECKPOINTS - 2U) % RNG_TRACE_CHECKPOINTS;
        u32 current = (s_rngCheckNext + RNG_TRACE_CHECKPOINTS - 1U) % RNG_TRACE_CHECKPOINTS;
        prevPacket = s_rngChecks[previous].packet;
        beginDraw = s_rngChecks[previous].draws;
        endDraw = s_rngChecks[current].draws;
    }
    ringStart = (s_rngTraceNext + RNG_TRACE_CAPACITY - s_rngTraceKept) % RNG_TRACE_CAPACITY;
    for (i = 0; i < s_rngTraceKept; ++i)
    {
        const struct NativeRngTraceEntry *entry = &s_rngTrace[(ringStart + i) % RNG_TRACE_CAPACITY];
        const char *site;
        if (entry->sequence <= beginDraw || entry->sequence > endDraw) continue;
        site = randomTraceBasename(entry->file);
        if (!strcmp(site, "gun.c")) ++gun;
        else if (strstr(site, "chr") || strstr(site, "ai.")) ++ai;
        else if (!strcmp(site, "propobj.c")) ++prop;
        else ++other;
        if (lastN < 4U) last[lastN++] = entry;
        else { last[0] = last[1];last[1] = last[2];last[2] = last[3];last[3] = entry; }
    }
    geXboxSnprintf(s_rngScreenText[0], RNG_SCREEN_WIDTH,
                   "007XB REPLAY RNG MISMATCH - ISO CAPTURE");
    geXboxSnprintf(s_rngScreenText[1], RNG_SCREEN_WIDTH,
                   "PACKET %u TICK %d EXPECT %02X GOT %02X", (unsigned)packet,
                   (int)tick, (unsigned)expected, (unsigned)actual);
    geXboxSnprintf(s_rngScreenText[2], RNG_SCREEN_WIDTH,
                   "FULL RNG STATE %08X%08X", (unsigned)(state >> 32), (unsigned)state);
    geXboxSnprintf(s_rngScreenText[3], RNG_SCREEN_WIDTH,
                   "FROM PACKET %u TO %u DRAWS %u", (unsigned)prevPacket,
                   (unsigned)packet, (unsigned)(endDraw - beginDraw));
    geXboxSnprintf(s_rngScreenText[4], RNG_SCREEN_WIDTH,
                   "CALLS GUN %u AI %u PROP %u OTHER %u", (unsigned)gun,
                   (unsigned)ai, (unsigned)prop, (unsigned)other);
    if (gunSync100 < 0)
        geXboxSnprintf(s_rngScreenText[5], RNG_SCREEN_WIDTH, "GUN SYNC NOT AVAILABLE");
    else
        geXboxSnprintf(s_rngScreenText[5], RNG_SCREEN_WIDTH, "GUN SYNC %d.%02d TICKS",
                       (int)(gunSync100 / 100), (int)(gunSync100 % 100));
    geXboxSnprintf(s_rngScreenText[6], RNG_SCREEN_WIDTH, "LAST FOUR RNG CONSUMERS");
    for (i = 0; i < 4U; ++i)
    {
        const struct NativeRngTraceEntry *entry = (i < lastN) ? last[i] : NULL;
        if (!entry) {
            s_rngScreenText[7U + i][0] = '\0';
        } else {
            geXboxSnprintf(s_rngScreenText[7U + i], RNG_SCREEN_WIDTH,
                "DRAW %u P%u T%d %.24s LINE %u", (unsigned)entry->sequence,
                (unsigned)entry->packet, (int)entry->tick,
                randomTraceBasename(entry->file), (unsigned)entry->line);
        }
    }
    /* x86/32-bit target publishes after all lines have been written. */
    s_rngScreenReady = 1;
}

u32 randomGetNextAt(const char *file, u32 line)
{
    struct NativeRngTraceEntry *entry;
    u64 before;
    u32 value;
    if (!s_rngTraceActive)
    {
        return randomAdvanceGlobal();
    }
    before = g_randomSeed;
    value = randomAdvanceGlobal();
    entry = &s_rngTrace[s_rngTraceNext];
    entry->before = before;
    entry->after = g_randomSeed;
    entry->file = file;
    entry->line = line;
    entry->sequence = ++s_rngTraceDraws;
    ++s_rngConsumerDraws[randomTraceConsumerFor(file)];
    entry->packet = s_rngTracePacket;
    entry->tick = g_GlobalTimer;
    s_rngTraceNext = (s_rngTraceNext + 1U) % RNG_TRACE_CAPACITY;
    if (s_rngTraceKept < RNG_TRACE_CAPACITY) ++s_rngTraceKept;
    return value;
}

u32 randomGetNext(void)
{
    return randomGetNextAt(NULL, 0);
}

void randomReplayTraceStart(void)
{
    s_rngScreenReady = 0;
    s_rngTraceActive = 0;
    s_rngTraceNext = s_rngTraceKept = s_rngTraceDraws = 0;
    s_rngTracePacket = s_rngCheckNext = s_rngCheckKept = 0;
    memset(s_rngConsumerDraws, 0, sizeof(s_rngConsumerDraws));
    memset(s_rngConsumerFileCache, 0, sizeof(s_rngConsumerFileCache));
    s_rngConsumerCacheNext = 0;
    s_rngTraceSeed = g_randomSeed;
    s_rngTraceDumped = 0;
    s_rngTraceActive = 1;
}

void randomReplayTracePacket(u32 packet, s32 tick)
{
    struct NativeRngTraceCheckpoint *check;
    if (!s_rngTraceActive) return;
    s_rngTracePacket = packet;
    check = &s_rngChecks[s_rngCheckNext];
    check->state = g_randomSeed;
    check->packet = packet;
    check->tick = tick;
    check->draws = s_rngTraceDraws;
    memcpy(check->consumerDraws, s_rngConsumerDraws, sizeof(s_rngConsumerDraws));
    s_rngCheckNext = (s_rngCheckNext + 1U) % RNG_TRACE_CHECKPOINTS;
    if (s_rngCheckKept < RNG_TRACE_CHECKPOINTS) ++s_rngCheckKept;
}

void randomReplayTraceStop(void)
{
    s_rngTraceActive = 0;
}

static const char *randomTraceBasename(const char *file)
{
    const char *base;
    if (!file || !*file) return "unknown";
    base = file;
    while (*file)
    {
        if (*file == '/' || *file == '\\') base = file + 1;
        ++file;
    }
    return *base ? base : "unknown";
}

void randomReplayTraceDump(void)
{
    u32 i;
    u32 first;
    u32 recentChecks, recentCalls;
    if (!s_rngTraceActive || s_rngTraceDumped) return;
    s_rngTraceActive = 0;
    s_rngTraceDumped = 1;
    GENativePlatform_TraceIssue(
        "DEMO RNG HISTORY seed=%08X%08X draws=%u kept=%u lost=%u checkpoints=%u capacity=%u\n",
        (unsigned)(s_rngTraceSeed >> 32), (unsigned)s_rngTraceSeed,
        (unsigned)s_rngTraceDraws, (unsigned)s_rngTraceKept,
        (unsigned)(s_rngTraceDraws - s_rngTraceKept),
        (unsigned)s_rngCheckKept, (unsigned)RNG_TRACE_CAPACITY);
    /* Packet-to-packet counts remain exact even when effects create more
     * than 256 RNG calls between checkpoints. The ring is for caller sites,
     * not the authoritative per-file tally. */
    if (s_rngCheckKept >= 2U)
    {
        const struct NativeRngTraceCheckpoint *previous =
            &s_rngChecks[(s_rngCheckNext + RNG_TRACE_CHECKPOINTS - 2U) % RNG_TRACE_CHECKPOINTS];
        const struct NativeRngTraceCheckpoint *current =
            &s_rngChecks[(s_rngCheckNext + RNG_TRACE_CHECKPOINTS - 1U) % RNG_TRACE_CHECKPOINTS];
        const u32 begin = previous->draws;
        const u32 total = current->draws - begin;
        u32 seen = 0;
        const u32 oldest = (s_rngTraceNext + RNG_TRACE_CAPACITY - s_rngTraceKept) % RNG_TRACE_CAPACITY;
        for (i = 0; i < s_rngTraceKept; ++i)
        {
            const struct NativeRngTraceEntry *e = &s_rngTrace[(oldest + i) % RNG_TRACE_CAPACITY];
            if (e->sequence > begin && e->sequence <= current->draws) ++seen;
        }
        GENativePlatform_TraceIssue(
            "DEMO RNG DELTA from_packet=%u to_packet=%u ticks=%d draws=%u observed=%u missing=%u gun=%u gunfire=%u chraction=%u chrai=%u chr=%u player=%u prop=%u explosion=%u glass=%u other=%u\n",
            (unsigned)previous->packet, (unsigned)current->packet,
            (int)(current->tick - previous->tick), (unsigned)total,
            (unsigned)seen, (unsigned)(total - seen),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_GUN] - previous->consumerDraws[RNG_CONSUMER_GUN]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_GUNFIRE] - previous->consumerDraws[RNG_CONSUMER_GUNFIRE]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_CHRACTION] - previous->consumerDraws[RNG_CONSUMER_CHRACTION]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_CHRAI] - previous->consumerDraws[RNG_CONSUMER_CHRAI]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_CHR] - previous->consumerDraws[RNG_CONSUMER_CHR]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_PLAYER] - previous->consumerDraws[RNG_CONSUMER_PLAYER]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_PROP] - previous->consumerDraws[RNG_CONSUMER_PROP]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_EXPLOSION] - previous->consumerDraws[RNG_CONSUMER_EXPLOSION]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_GLASS] - previous->consumerDraws[RNG_CONSUMER_GLASS]),
            (unsigned)(current->consumerDraws[RNG_CONSUMER_OTHER] - previous->consumerDraws[RNG_CONSUMER_OTHER]));
    }
    if (s_rngCheckKept >= 2U)
    {
        enum { MAX_SITES = 40, TOP_SITES = 8 };
        struct RngSiteCount {const char *file; u32 line; u32 count;};
        struct RngSiteCount sites[MAX_SITES];
        u32 siteCount = 0, next, j;
        u32 selected[TOP_SITES];
        u32 selectedCount = 0;
        char text[1536];
        int used;
        const struct NativeRngTraceCheckpoint *previous =
            &s_rngChecks[(s_rngCheckNext + RNG_TRACE_CHECKPOINTS - 2U) % RNG_TRACE_CHECKPOINTS];
        const struct NativeRngTraceCheckpoint *current =
            &s_rngChecks[(s_rngCheckNext + RNG_TRACE_CHECKPOINTS - 1U) % RNG_TRACE_CHECKPOINTS];
        const u32 firstSite = (s_rngTraceNext + RNG_TRACE_CAPACITY - s_rngTraceKept) % RNG_TRACE_CAPACITY;
        u32 seen = 0, overflowSites = 0;
        for (i = 0; i < s_rngTraceKept; ++i)
        {
            const struct NativeRngTraceEntry *entry =
                &s_rngTrace[(firstSite + i) % RNG_TRACE_CAPACITY];
            if (entry->sequence <= previous->draws || entry->sequence > current->draws) continue;
            ++seen;
            for (j = 0; j < siteCount; ++j)
            {
                if (sites[j].line == entry->line &&
                    !strcmp(randomTraceBasename(sites[j].file),randomTraceBasename(entry->file))) break;
            }
            if (j < siteCount) ++sites[j].count;
            else if (siteCount < MAX_SITES)
            {
                sites[siteCount].file = entry->file;
                sites[siteCount].line = entry->line;
                sites[siteCount].count = 1;
                ++siteCount;
            }
            else ++overflowSites;
        }
        used = geXboxSnprintf(text,sizeof(text),
            "DEMO RNG SITES from_packet=%u to_packet=%u partial=%u unique=%u overflow=%u",
            (unsigned)previous->packet,(unsigned)current->packet,
            (unsigned)((current->draws - previous->draws) != seen),
            (unsigned)siteCount,(unsigned)overflowSites);
        if (used >= 0 && (u32)used < sizeof(text))
        {
            while (selectedCount < TOP_SITES && selectedCount < siteCount)
            {
                u32 best = MAX_SITES;
                for (i = 0; i < siteCount; ++i)
                {
                    int already = 0;
                    for (j = 0; j < selectedCount; ++j)
                        if (selected[j] == i) already = 1;
                    if (!already && (best == MAX_SITES || sites[i].count > sites[best].count)) best = i;
                }
                if (best == MAX_SITES) break;
                selected[selectedCount++] = best;
                next = (u32)geXboxSnprintf(text + used, sizeof(text) - (u32)used,
                    " %.40s:%u=%u",randomTraceBasename(sites[best].file),
                    (unsigned)sites[best].line,(unsigned)sites[best].count);
                if (next >= sizeof(text) - (u32)used) break;
                used += (int)next;
            }
            GENativePlatform_TraceIssue("%s\n",text);
        }
    }
    recentChecks = GE_XBOX_RNG_FULL_TRACE ? s_rngCheckKept :
        (s_rngCheckKept < RNG_TRACE_COMPACT_CHECKS ? s_rngCheckKept : RNG_TRACE_COMPACT_CHECKS);
    first = (s_rngCheckNext + RNG_TRACE_CHECKPOINTS - recentChecks) %
        RNG_TRACE_CHECKPOINTS;
    for (i = 0; i < recentChecks; ++i)
    {
        const struct NativeRngTraceCheckpoint *check =
            &s_rngChecks[(first + i) % RNG_TRACE_CHECKPOINTS];
        GENativePlatform_TraceIssue(
            "DEMO RNG CHECK packet=%u tick=%d draws=%u state=%08X%08X\n",
            (unsigned)check->packet, (int)check->tick, (unsigned)check->draws,
            (unsigned)(check->state >> 32), (unsigned)check->state);
    }
    recentCalls = GE_XBOX_RNG_FULL_TRACE ? s_rngTraceKept :
        (s_rngTraceKept < RNG_TRACE_COMPACT_CALLS ? s_rngTraceKept : RNG_TRACE_COMPACT_CALLS);
    first = (s_rngTraceNext + RNG_TRACE_CAPACITY - recentCalls) %
        RNG_TRACE_CAPACITY;
    for (i = 0; i < recentCalls; i += 4U)
    {
        char text[1536];
        int used = geXboxSnprintf(text, sizeof(text), "DEMO RNG CALLS");
        u32 j;
        if (used < 0 || (unsigned)used >= sizeof(text)) return;
        for (j = i; j < recentCalls && j < i + 4U; ++j)
        {
            const struct NativeRngTraceEntry *entry =
                &s_rngTrace[(first + j) % RNG_TRACE_CAPACITY];
            int length = geXboxSnprintf(text + used, sizeof(text) - used,
                " [n=%u p=%u t=%d site=%.80s:%u pre=%08X%08X post=%08X%08X]",
                (unsigned)entry->sequence, (unsigned)entry->packet, (int)entry->tick,
                randomTraceBasename(entry->file), (unsigned)entry->line,
                (unsigned)(entry->before >> 32), (unsigned)entry->before,
                (unsigned)(entry->after >> 32), (unsigned)entry->after);
            if (length < 0 || (unsigned)length >= sizeof(text) - used)
            {
                GENativePlatform_TraceIssue("DEMO RNG HISTORY_FORMAT_FAILED\n");
                return;
            }
            used += length;
        }
        GENativePlatform_TraceIssue("%s\n", text);
    }
}
#endif

/* Set the seed (the .s adds 1 before storing). */
/* Manual hex parse (u64-safe): MinGW's strtoull returns the sign-extended low
 * 32 bits for values above LLONG_MAX. Stops at first non-hex char. */
static u64 randomParseHex(const char *s)
{
    u64 v = 0;

    while (*s)
    {
        char c = *s++;

        v <<= 4;
        if (c >= '0' && c <= '9')      { v |= (u64)(c - '0'); }
        else if (c >= 'a' && c <= 'f') { v |= (u64)(c - 'a' + 10); }
        else if (c >= 'A' && c <= 'F') { v |= (u64)(c - 'A' + 10); }
        else                           { break; }
    }
    return v;
}

void randomSetSeed(u32 seed)
{
#ifdef PORT
    /* GE_RSEED=<hex64>: test-only boot-seed override. boss.c seeds the PRNG
     * from osGetCount() (wall-clock on PC; RSP timer on N64), so the boot
     * seed differs every run -- with or without GE_DETERM, because the
     * virtual clock's pre-task advance count is still real-time dependent.
     * This pins it for determinism tests (E1) and N64 parity tests (E-B). */
    {
        const char *e = getenv("GE_RSEED");

        if (e && e[0])
        {
            g_randomSeed = randomParseHex(e);
            osSyncPrintf("D318R: SETSEED OVERRIDDEN by GE_RSEED -> %016llx\n",
                         (unsigned long long)g_randomSeed);
            return;
        }
    }
#endif
    g_randomSeed = (u64)seed + 1;   /* daddiu $a0,$a0,1; sd $a0, g_randomSeed */
#ifdef PORT
    if (getenv("GE_D318R") != NULL)
        osSyncPrintf("D318R: SETSEED arg=%08x -> %016llx\n",
                     seed, (unsigned long long)g_randomSeed);
#endif
}

/*
 * Same transform as randomGetNext, but applied to *seed (a caller-owned u64)
 * instead of the global. Mirrors randomGetNextFrom in random.s.
 */
u32 randomGetNextFrom(u64 *seed)
{
    u64 a3 = *seed;          /* ld     $a3, ($a0) */
    u64 a1, a2;

    a2 = dsll32(a3, 0x1f);   /* dsll32 $a2, $a3, 0x1f */
    a1 = a3 << 0x1f;         /* dsll   $a1, $a3, 0x1f */
    a2 = a2 >> 0x1f;         /* dsrl   $a2, $a2, 0x1f */
    a1 = dsrl32(a1, 0);      /* dsrl32 $a1, $a1, 0    */
    a3 = dsll32(a3, 0xc);    /* dsll32 $a3, $a3, 0xc  */
    a2 = a2 | a1;            /* or     $a2, $a2, $a1  */
    a3 = dsrl32(a3, 0);      /* dsrl32 $a3, $a3, 0    */
    a2 = a2 ^ a3;            /* xor    $a2, $a2, $a3  */
    a3 = a2 >> 0x14;         /* dsrl   $a3, $a2, 0x14 */
    a3 = a3 & 0xfff;         /* andi   $a3, $a3, 0xfff*/
    a3 = a3 ^ a2;            /* xor    $a3, $a3, $a2  */

    *seed = a3;              /* sd     $a3, ($a0) */
    return (u32)a3;          /* v0 = a3 & 0xFFFFFFFF */
}

/* --- Character-object PRNG (src/game/chrObjRandom.s) ---------------------- */
/* Same xorshift as randomGetNext, separate state. Seed is two .words in the   */
/* .s: 0xAB8D9F77 (hi) / 0x81280783 (lo) — same initial value as g_randomSeed. */

u64 g_chrObjRandomSeed = 0xAB8D9F7781280783ULL;

u32 chrObjRandomGetNext(void)
{
    u64 a0 = g_chrObjRandomSeed;
    u64 a1, a2;

    a2 = dsll32(a0, 0x1f);   /* dsll32 $a2, $a0, 0x1f */
    a1 = a0 << 0x1f;         /* dsll   $a1, $a0, 0x1f */
    a2 = a2 >> 0x1f;         /* dsrl   $a2, $a2, 0x1f */
    a1 = dsrl32(a1, 0);      /* dsrl32 $a1, $a1, 0    */
    a0 = dsll32(a0, 0xc);    /* dsll32 $a0, $a0, 0xc  */
    a2 = a2 | a1;            /* or     $a2, $a2, $a1  */
    a0 = dsrl32(a0, 0);      /* dsrl32 $a0, $a0, 0    */
    a2 = a2 ^ a0;            /* xor    $a2, $a2, $a0  */
    a0 = a2 >> 0x14;         /* dsrl   $a0, $a2, 0x14 */
    a0 = a0 & 0xfff;         /* andi   $a0, $a0, 0xfff*/
    a0 = a0 ^ a2;            /* xor    $a0, $a0, $a2  */

    g_chrObjRandomSeed = a0; /* sd     $a0, g_chrObjRandomSeed */
    return (u32)a0;          /* v0 = a0 & 0xFFFFFFFF            */
}

void chrObjRandomSetSeed(u32 seed)
{
    g_chrObjRandomSeed = (u64)seed + 1;   /* daddiu $a0,$a0,1; sd $a0, ... */
}
