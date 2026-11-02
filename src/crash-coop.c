// crash-coop.dll - two-player online co-op for Crash Bandicoot N. Sane Trilogy (Steam build 2964270).
// Injected into the running game by coop.ps1 (or the CrashCoop.ps1 launcher). Shows the friend as a puppet Crash (or
// Coco), replays their animations, and shares broken crates and wumpa fruit. Steam P2P through the game's own Steam.
// Config <dll dir>\coop-<pid>.cfg (written by coop.ps1), one line:
//   "<name> udp <bind ip> <listen port> <peer ip> <peer port>"     (lab: two copies on one PC)
//   "<name> steam <peer SteamID64>"                                 (real play: Steam P2P via the game's own Steam)
//   "<name> steamprobe"                                              (only checks the Steam interfaces, then idles)
// The "Crash" prototype CActor is found automatically (CActor vtable RVA 0x1592220, name pointer "Crash").
// Per frame: send own player state (CActor+0x20 position, xform+0x60 euler, xform+0x10 quaternion) at ~60 Hz;
// puppet (spawned once with SpawnEntity 0x772F90 on the game thread) gets the peer's state with the teleport recipe
// (xform+0x7C = 1, actor+0x38 |= 0x400 | 0x800). Stop file: <dll dir>\coop.stop. Log: <dll dir>\coop-<name>.log
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <math.h>
#include <mmsystem.h>
#include <string.h>

#define SPAWN_RVA   0x772F90
#define HOOK_RVA    0xC05A60
#define MGR_RVA     0x1A665E8
#define CACTOR_VT   0x1592220
static char g_dir[MAX_PATH];   /* folder of this DLL: config, log and stop file live next to it */

#define MAGIC       0x504F4343u
#define STEAM_CHAN  7

#pragma pack(push, 1)
// v2: session = random id per mod start (a restarted peer is accepted at once, seq restarts at 1);
//     level = FNV-1a of the lower-case level name (puppet shown only when both players are in the same level)
// v5: animation = the Havok behavior events fired on the sender's Crash (last NEV, each with a running sequence
//     number so a lost packet loses nothing) + the sender's RunSpeedMultiplier (#21) and TurboMotion (#18) values
#define NEV 32
typedef struct { unsigned magic; unsigned short version; unsigned seq; unsigned session; unsigned level; float pos[3]; float euler[3]; float quat[4]; unsigned char input[48];
    unsigned evSeq; short evId[NEV]; float runSpeed; int turbo; int groundState;
    unsigned crSeq; float crPos[8][3]; unsigned fruitTotal; char hero[8]; } Packet;
// v6: crates = positions of the last 8 crates the sender broke, with a running sequence number (like the events)
// v7: fruitTotal = wumpa fruit the sender gained since its mod started (shared fruit counter)
#define NCR 8
// v8: hero = the hero the sender plays ("Crash" or "Coco")
#define PKT_VERSION 8
// v3: input = the identified ActorInput fields (buttons + stick, never pointers) so the puppet animates by itself
static const int IN_OFF[6] = { 0x20, 0x78, 0x3C, 0xA0, 0xC0, 0xD0 }, IN_LEN[6] = { 8, 8, 4, 12, 8, 8 };
static void read_input(DWORD64 ai, unsigned char *o);
static void write_input(DWORD64 ai, const unsigned char *in);
#pragma pack(pop)
static void arm_all(DWORD skip);
typedef DWORD64 (*SpawnFn)(DWORD64 proto, float *pos, float *rot, DWORD64 a4, DWORD64 a5, DWORD a6, DWORD64 a7, DWORD64 a8);

// --- old Steamworks flat API (SDK of the game's steam_api64.dll): first argument is the interface pointer ---
typedef void *(*CreateInterfaceFn)(const char *ver);
typedef int (*GetHandleFn)(void);
typedef void *(*GetIfaceFn)(void *client, int hUser, int hPipe, const char *ver);
typedef unsigned long long (*GetSteamIDFn)(void *user);
typedef BOOL (*SendP2PFn)(void *net, unsigned long long to, const void *data, unsigned size, int sendType, int chan);
typedef BOOL (*AvailP2PFn)(void *net, unsigned *size, int chan);
typedef BOOL (*ReadP2PFn)(void *net, void *dst, unsigned cap, unsigned *size, unsigned long long *from, int chan);
typedef BOOL (*AcceptP2PFn)(void *net, unsigned long long who);
typedef BOOL (*RelayFn)(void *net, BOOL allow);
static struct { void *net, *user; SendP2PFn send; AvailP2PFn avail; ReadP2PFn read; AcceptP2PFn accept; unsigned long long me, peer; } S;

static DWORD64 g_base, g_proto, g_poolOwner;
static volatile DWORD64 g_puppet;
static volatile LONG g_spawnState;
// --- animation (v11) -------------------------------------------------------------------------------------------
// Crash's animation is a Havok behavior graph driven by EVENTS (run = event 81 every frame, stop 86, spin 193, crouch
// 301, jump 82/87/83...) that the game fires only for the character a player controls, through
// FireEventById = exe+0x33FEA0(CBehaviorCharacter, id). Sender: a persistent execute breakpoint (DR1) on that function
// records every event fired on our own Crash. Receiver: the events are re-fired on the puppet at exe+0x3733F0 (DR3),
// the per-character behavior variable sync (rcx = CBehaviorCharacter, once per character per frame on the game thread,
// the same stage the game fires its own events in; firing from SetWorldPosition instead crashed the game). Right after
// that sync returns (exe+0x366836, DR2) the friend's RunSpeedMultiplier / TurboMotion are written over the puppet's
// (the game writes 0 there for a character nobody controls, which freezes the run cycle).
#define FIRE_RVA  0x33FEA0
#define SYNC_RVA  0x3733F0
#define POST_RVA  0x366836
#define BCOMP_VT  0x150C710   /* CBehaviorComponent; +0x38 = CBehaviorCharacter */
static volatile DWORD64 g_myBC, g_pupBC, g_curBC;
static volatile LONG g_capHead; static short g_cap[256];                 /* events captured on our Crash */
static volatile LONG g_rqHead, g_rqTail; static short g_rq[256];         /* events to replay on the puppet */
static volatile float g_peerRunSpeed; static volatile int g_peerTurbo, g_animOn, g_peerGround; static volatile LONG g_forced;
static volatile LONG g_replayed, g_captured; static volatile int g_noEvents; static short g_replayLog[256];
static volatile DWORD64 g_dr0; static volatile int g_dr0on;   /* one-shot spawn hook (DR0), kept apart from DR1-3 */
static float g_spawnPos[3], g_rot[3];
static Packet g_peerPkt; static volatile LONG g_peerValid, g_rx, g_rxRejected; static unsigned g_lastSeq;
static CRITICAL_SECTION g_cs;
static SOCKET g_sock = INVALID_SOCKET; static struct sockaddr_in g_peerAddr;
static int g_steam;
static FILE *g_log;

// --- crates (v12) -------------------------------------------------------------------------------------------------
// Every crate has a CDestructibleComponent (vtable RVA 0x14D1458, owner entity at +0x18, entity position at +0x20).
// A broken crate's component is freed (vtable overwritten), so the sender polls its list of crates and sends the
// position of each one that disappears. The receiver finds its own crate at that position and breaks it with the
// game's damage entry exe+0x4D63A0(CDamageData *def, attacker, target) on the game thread, with its own player as
// attacker and a "bounce" CDamageData (vtable RVA 0x13FF3D8, [+0x10]+0x78 = "bounce"): the game then does everything
// a real jump does (fruit, crate count, Aku Aku mask...). Calling only the crate's stage function 0x468130 was not
// enough. Lists are rebuilt by a background memory scan when the player changes and every 15 s.
#define DESTR_VT   0x14D1458
#define DDATA_VT   0x13FF3D8
#define DAMAGE_RVA 0x4D63A0
#define MAXCR 1024
static CRITICAL_SECTION g_crCs;
static DWORD64 g_crComp[MAXCR]; static float g_crP[MAXCR][3]; static int g_crN; static volatile DWORD64 g_bounceDef;
static volatile LONG g_myCrHead; static float g_myCr[256][3];          /* crates we broke (sent) */
static volatile LONG g_inCrHead, g_inCrTail; static float g_inCr[64][3]; static unsigned g_inCrLevel[64];
static volatile DWORD64 g_hitDef, g_hitAtt, g_hitTgt, g_hitComp; static volatile LONG g_hitReq;   /* 1 = to do, 2 = done */
static volatile LONG g_crSent, g_crRemote, g_crMiss;

// --- Coco puppet (v14) ------------------------------------------------------------------------------------------------
// With a file coop.coco next to the dll, the friend's puppet is Coco. Coco's hero pack is not loaded while we play
// Crash, so the mod asks for it on the game thread: exe+0x4EBDB0([exe+0x1A61C88], &req, igStringRef "Coco",
// pool(chunk 92), 1), i.e. into the local hero's memory pool (exe+0xDF0AD0(92), ~149 MB, the other hero pools 93+ have
// no memory without a real second player). The game logs "Loaded chunk 'coco:Hero_Crash_data_92'" and an inert "Coco"
// CActor appears next to Crash's; the puppet is cloned from it. Asked again in every level (the hero pool is reloaded
// on level changes). The request's reference is kept, never released.
static volatile LONG g_cocoReq; static volatile DWORD64 g_cocoReqObj; static int g_wantCoco;
// Replaying Crash's behavior event ids on the Coco puppet made it shatter and vanish (lab, 2026-11-02: events 82 87 83,
// a jump): Coco's graph numbers its events differently. Until events are translated by name, the Coco puppet gets no
// events and animates from the friend's buttons only (it then runs and jumps and stays alive).
static volatile int g_pupCoco;   /* 1 = puppet hero differs from the friend's: no events */
static char g_pupHero[8], g_pupFor[8], g_packName[8] = "Coco";
// our own hero = slot 0's wanted hero name in CPlayerEntityManager (+0x100, a char*), "Crash" or "Coco"
static int safe_read(DWORD64 a, void *dst, SIZE_T n); static DWORD64 rd(DWORD64 a);
static void my_hero(char *o) { DWORD64 pem = rd(g_base + 0x1A68600), s = pem ? rd(pem + 0x100) : 0; o[0] = 0;
    if (!s || !safe_read(s, o, 7)) strcpy(o, "Crash"); o[7] = 0; for (int i = 0; i < 8; i++) if (o[i] && (o[i] < 32 || o[i] > 126)) { o[i] = 0; break; } if (!o[0]) strcpy(o, "Crash"); }

// --- shared wumpa counter (v13) -------------------------------------------------------------------------------------
// The real counter is a CGameVariableValue (vtable RVA 0x155F090, int at +0x10); writing it works (the HUD follows,
// the game keeps counting from there). Which of the ~hundreds of game variables it is: the player's HUD script
// component (CVscComponent 0x13BBBD0 whose [+0x38]+0x10 = "Scripts.Graph.CrashBandicoot_HUD") keeps a display copy at
// +0x80 (writing that copy does nothing: the HUD rewrites it). The mod keeps the variables whose value equals the
// copy each time the copy changes, until one is left.
#define GVV_VT   0x155F090
#define VSC_VT   0x13BBBD0
#define MAXGV 8192
static DWORD64 g_gv[MAXGV]; static int g_gvN;   /* all CGameVariableValue (from the scanner), under g_crCs */
static volatile unsigned g_peerFruit; static volatile int g_peerFruitValid;

static int stop_requested(void) { char p[MAX_PATH]; sprintf(p, "%scoop.stop", g_dir); return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int readable(DWORD64 a, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!a || !VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return a + n <= (DWORD64)mi.BaseAddress + mi.RegionSize;
}
static int safe_read(DWORD64 a, void *dst, SIZE_T n);
static int safe_write(DWORD64 a, const void *src, SIZE_T n) { SIZE_T put = 0; return WriteProcessMemory(GetCurrentProcess(), (void *)a, src, n, &put) && put == n; }
// every read of game memory goes through ReadProcessMemory: memory freed during a level unload makes the call fail
// instead of faulting (VirtualQuery-then-dereference races with the game's own frees)
static DWORD64 rd(DWORD64 a) { DWORD64 v = 0; return (a && safe_read(a, &v, 8)) ? v : 0; }
static DWORD64 player_actor(void) {
    DWORD64 mgr = rd(g_base + MGR_RVA), list = mgr ? rd(mgr + 0x10) : 0, data = list ? rd(list + 0x20) : 0;
    DWORD64 player = data ? rd(data) : 0, handle = player ? rd(player + 0x10) : 0;
    return handle ? rd(handle + 0x28) : 0;
}
// actor -> its CBehaviorComponent (component list at actor+0x18: count +0x10, array +0x20) -> +0x38 CBehaviorCharacter
static DWORD64 behavior_char(DWORD64 actor) {
    DWORD64 cl = actor ? rd(actor + 0x18) : 0, arr = cl ? rd(cl + 0x20) : 0; int n = 0;
    if (cl) safe_read(cl + 0x10, &n, 4);
    for (int i = 0; i < n && i < 200 && arr; i++) { DWORD64 o = rd(arr + 8 * i); if (o && rd(o) == g_base + BCOMP_VT) return rd(o + 0x38); }
    return 0;
}

// Safe copy: ReadProcessMemory on our own process fails cleanly instead of faulting when the game frees/decommits
// memory between VirtualQuery and the read (a plain pointer walk here crashed both test copies).
static int safe_read(DWORD64 a, void *dst, SIZE_T n) { SIZE_T got = 0; return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, dst, n, &got) && got == n; }

// find the CActor whose name (a char* among its first 0x100 bytes) is exactly g_protoName ("Crash", or "Coco")
static char g_protoName[8] = "Crash";
static DWORD64 find_prototype(void) {
    DWORD64 vt = g_base + CACTOR_VT, a = 0x10000, self = player_actor(); MEMORY_BASIC_INFORMATION mi;
    // skip our own thread's stack: it holds the vtable value and the hero name we look for (v15 "found" a prototype
    // there and the spawn failed)
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb(); DWORD64 stLo = (DWORD64)tib->StackLimit, stHi = (DWORD64)tib->StackBase;
    static unsigned char chunk[1 << 20];
    while (a < 0x7FFFFFFF0000ull && VirtualQuery((void *)a, &mi, sizeof mi)) {
        DWORD64 b = (DWORD64)mi.BaseAddress, s = mi.RegionSize; a = b + s;
        if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || (mi.Protect & 0xFF) != PAGE_READWRITE || (mi.Protect & PAGE_GUARD)) continue;
        if (b < stHi && b + s > stLo) continue;
        for (DWORD64 off = 0; off < s; off += sizeof chunk) {
            SIZE_T len = (SIZE_T)((s - off) < sizeof chunk ? (s - off) : sizeof chunk);
            if (!safe_read(b + off, chunk, len)) continue;
            for (SIZE_T i = 0; i + 0x100 <= len; i += 8) {
                if (*(DWORD64 *)(chunk + i) != vt || b + off + i == self) continue;   // never clone the live player
                {   // the prototype is inert: no components (live Crashes, incl. earlier puppets, have ~75)
                    DWORD64 cl = *(DWORD64 *)(chunk + i + 0x18); int nc = 0;
                    if (cl && safe_read(cl + 0x10, &nc, 4) && nc != 0) continue;
                }
                // the name pointer lives in the entity itself or in its data object (entity+0x40)
                DWORD64 bases[2] = { b + off + i, *(DWORD64 *)(chunk + i + 0x40) };
                for (int j = 0; j < 2; j++) {
                    unsigned char obj[0x100];
                    if (!bases[j] || !safe_read(bases[j], obj, sizeof obj)) continue;
                    for (int k = 0; k < 0x100; k += 8) {
                        DWORD64 str = *(DWORD64 *)(obj + k); char name[8]; size_t ln = strlen(g_protoName) + 1;
                        if (str > 0x10000 && safe_read(str, name, ln) && memcmp(name, g_protoName, ln) == 0) return b + off + i;
                    }
                }
            }
        }
    }
    return 0;
}

// one memory pass: every live crate (component + its entity position) and one "bounce" CDamageData
static volatile DWORD64 g_scanFor; static volatile LONG g_scanReq;
static DWORD WINAPI crate_scanner(LPVOID arg) {
    (void)arg; static unsigned char chunk[1 << 20]; static DWORD64 comps[MAXCR]; static float pos[MAXCR][3]; static DWORD64 gvs[MAXGV];
    while (!stop_requested()) {
        if (!g_scanReq) { Sleep(50); continue; }
        DWORD64 forActor = g_scanFor, dvt = g_base + DESTR_VT, ddv = g_base + DDATA_VT, gvv = g_base + GVV_VT, def = 0, a = 0x10000; int n = 0, ng = 0;
        MEMORY_BASIC_INFORMATION mi;
        while (a < 0x7FFFFFFF0000ull && VirtualQuery((void *)a, &mi, sizeof mi)) {
            DWORD64 b = (DWORD64)mi.BaseAddress, s = mi.RegionSize; a = b + s;
            if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || (mi.Protect & 0xFF) != PAGE_READWRITE || (mi.Protect & PAGE_GUARD)) continue;
            for (DWORD64 off = 0; off < s; off += sizeof chunk) {
                SIZE_T len = (SIZE_T)((s - off) < sizeof chunk ? (s - off) : sizeof chunk);
                if (!safe_read(b + off, chunk, len)) continue;
                for (SIZE_T i = 0; i + 8 <= len; i += 8) {
                    DWORD64 v = *(DWORD64 *)(chunk + i);
                    if (v == dvt && n < MAXCR) {
                        // a real crate component is listed in its entity's component list (entity+0x18: count +0x10,
                        // array +0x20); copies of the vtable value on thread stacks are not
                        DWORD64 me = b + off + i, e = rd(me + 0x18), cl = e ? rd(e + 0x18) : 0, arr = cl ? rd(cl + 0x20) : 0; float p[3]; int nc = 0, member = 0;
                        if (cl) safe_read(cl + 0x10, &nc, 4);
                        for (int k = 0; k < nc && k < 64 && arr; k++) if (rd(arr + 8 * k) == me) { member = 1; break; }
                        if (member && safe_read(e + 0x20, p, 12) && (p[0] != 0 || p[1] != 0 || p[2] != 0)) { comps[n] = me; memcpy(pos[n], p, 12); n++; }
                    } else if (v == gvv && ng < MAXGV) {
                        gvs[ng++] = b + off + i;
                    } else if (v == ddv && !def) {
                        DWORD64 sub = rd(b + off + i + 0x10), str = sub ? rd(sub + 0x78) : 0; char nm[7];
                        if (str && safe_read(str, nm, 7) && !memcmp(nm, "bounce", 7)) def = b + off + i;
                    }
                }
            }
        }
        EnterCriticalSection(&g_crCs);
        if (forActor == g_scanFor && forActor == player_actor()) {
            // a crate in the old list but missing from the new one broke during the scan: the detection pass of the
            // worker sees it first because it runs on the old list until this swap (small window, accepted)
            memcpy(g_crComp, comps, n * sizeof comps[0]); memcpy(g_crP, pos, n * sizeof pos[0]); g_crN = n;
            if (def) g_bounceDef = def;
            memcpy(g_gv, gvs, ng * sizeof gvs[0]); g_gvN = ng;
        }
        LeaveCriticalSection(&g_crCs);
        fprintf(g_log, "crate scan: %d crates, bounce def %s, %d game variables\n", n, def ? "found" : "missing", ng); fflush(g_log);
        g_scanReq = 0;
    }
    return 0;
}

// Debug registers of every game thread: DR0 = one-shot spawn hook (when g_dr0on), DR1-3 = the persistent animation
// hooks (when g_animOn). Re-applied every 500 ms so threads created later get them too.
static void set_dr(DWORD tid) {
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!t) return;
    SuspendThread(t);
    CONTEXT c; ZeroMemory(&c, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c)) {
        int on = g_animOn;
        c.Dr0 = g_dr0on ? g_dr0 : 0; c.Dr1 = on ? g_base + FIRE_RVA : 0; c.Dr2 = on ? g_base + POST_RVA : 0; c.Dr3 = on ? g_base + SYNC_RVA : 0;
        c.Dr6 = 0; c.Dr7 = (g_dr0on ? 1 : 0) | (on ? (4 | 0x10 | 0x40) : 0);   /* all execute breakpoints (R/W = 00, LEN = 00) */
        SetThreadContext(t, &c);
    }
    ResumeThread(t); CloseHandle(t);
}
static void arm_all(DWORD skip) {
    DWORD pid = GetCurrentProcessId();
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (Thread32First(s, &te)) do {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != GetCurrentThreadId() && te.th32ThreadID != skip) set_dr(te.th32ThreadID);
    } while (Thread32Next(s, &te));
    CloseHandle(s);
}
static void set_all(DWORD64 addr, DWORD64 dr7, DWORD skip) { g_dr0 = addr; g_dr0on = (dr7 & 1) != 0; arm_all(skip); }
static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    CONTEXT *c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if ((c->Dr6 & 2) && c->Rip == g_base + FIRE_RVA) {          // an event fired: record it if it is for our Crash
        c->Dr6 = 0; c->EFlags |= 0x10000;
        if (c->Rcx && c->Rcx == g_myBC) { LONG i = g_capHead; g_cap[i & 255] = (short)c->Rdx; g_capHead = i + 1; g_captured++; }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((c->Dr6 & 8) && c->Rip == g_base + SYNC_RVA) {          // per-character behavior sync: replay events on the puppet
        c->Dr6 = 0; c->EFlags |= 0x10000; g_curBC = c->Rcx;
        if (g_cocoReq == 1) {  // load the Coco hero pack next to Crash (see "Coco puppet" below)
            DWORD64 s = 0, req = 0; g_cocoReq = 2;
            ((void (*)(DWORD64 *, const char *))(g_base + 0xDCE380))(&s, g_packName);
            DWORD64 pool = ((DWORD64 (*)(unsigned))(g_base + 0xDF0AD0))(92);
            DWORD64 loader = rd(g_base + 0x1A61C88);
            if (s && pool && loader) ((DWORD64 * (*)(DWORD64, DWORD64 *, DWORD64, DWORD64, int))(g_base + 0x4EBDB0))(loader, &req, s, pool, 1);
            g_cocoReqObj = req;
        }
        if (g_hitReq == 1) {   // a crate the friend broke: hit it here, on the game thread, like a jump would
            DWORD64 vt = 0; g_hitReq = 2;
            if (safe_read(g_hitComp, &vt, 8) && vt == g_base + DESTR_VT && g_hitAtt == player_actor())
                ((unsigned char (*)(DWORD64, DWORD64, DWORD64))(g_base + DAMAGE_RVA))(g_hitDef, g_hitAtt, g_hitTgt);
        }
        if (c->Rcx && c->Rcx == g_pupBC) {
            int n = 0;
            while (g_rqTail != g_rqHead && n < 64) { short id = g_rq[g_rqTail & 255]; g_rqTail++; n++;
                if (g_noEvents || g_pupCoco) continue;   /* Crash event ids killed the Coco puppet (other graph) */
                g_replayLog[g_replayed & 255] = id;
                ((unsigned long long (*)(DWORD64, int))(g_base + FIRE_RVA))(c->Rcx, id); g_replayed++; }
            // State regulation: the puppet's own logic pulls its graph back to idle after a lone run event, so while
            // the friend runs (GroundLocomotionStateID #24 = 2) re-fire the run event (81) until the puppet runs too,
            // and the stop event (86) when the friend is back to idle.
            if (!g_noEvents && !g_pupCoco) {
                DWORD64 hk = *(DWORD64 *)(c->Rcx + 0x30), g = hk ? *(DWORD64 *)(hk + 0x80) : 0, vs = g ? *(DWORD64 *)(g + 0x110) : 0, w = vs ? *(DWORD64 *)(vs + 0x10) : 0;
                int nw = vs ? *(int *)(vs + 0x18) : 0;
                if (w && nw > 24) {
                    int mine = *(int *)(w + 24 * 4), want = g_peerGround;
                    if (want == 2 && mine != 2) { ((unsigned long long (*)(DWORD64, int))(g_base + FIRE_RVA))(c->Rcx, 81); g_forced++; }
                    else if (want == 0 && mine == 2) { ((unsigned long long (*)(DWORD64, int))(g_base + FIRE_RVA))(c->Rcx, 86); g_forced++; }
                }
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((c->Dr6 & 4) && c->Rip == g_base + POST_RVA) {          // after the game's variable sync: friend's run speed
        c->Dr6 = 0; c->EFlags |= 0x10000;
        DWORD64 bc = g_curBC;
        if (bc && bc == g_pupBC) {
            DWORD64 hk = *(DWORD64 *)(bc + 0x30), g = hk ? *(DWORD64 *)(hk + 0x80) : 0, vs = g ? *(DWORD64 *)(g + 0x110) : 0, w = vs ? *(DWORD64 *)(vs + 0x10) : 0;
            int nw = vs ? *(int *)(vs + 0x18) : 0;
            if (w && nw > 21) { float s = g_peerRunSpeed; int t = g_peerTurbo; memcpy((void *)(w + 21 * 4), &s, 4); memcpy((void *)(w + 18 * 4), &t, 4); }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0; c->Dr7 &= ~1ull; c->EFlags |= 0x10000;
    if (c->Rip == g_base + HOOK_RVA && InterlockedCompareExchange(&g_spawnState, 2, 1) == 1) {
        // 4th argument = "pool owner": core 0x772CB0 allocates the new entity in the memory pool of this object
        // (else in the prototype's pool = the permanent character pack). Use the live player so the puppet lives in
        // the level's pool and is torn down with the level (pool 0 -> crash at exe+0xFFE0F7 on every level change).
        DWORD64 e = ((SpawnFn)(g_base + SPAWN_RVA))(g_proto, g_spawnPos, g_rot, g_poolOwner, 0, 0, 0, 0);
        // Crash 3's prototype spawns WITHOUT an igEntityTransform (entity+0x30 = 0). The engine creates one on demand
        // in SetRotation (0xC05860 calls 0xBFCFE0(entity) when +0x30 is null); do the same here, on the game thread.
        if (e && !rd(e + 0x30)) ((void (*)(DWORD64))(g_base + 0xBFCFE0))(e);
        // the game's own spawn wrappers (0x772700...) take a reference right away: AddRef = lock xadd [entity+8], 1
        // (exe+0xE557B0). Without it the spawned entity had no owner, and every map -> level transition crashed in
        // the game at exe+0xFFE0F7.
        // AddRef disabled for the pool-owner test: with the extra reference the level teardown crashed inside the
        // release path (exe+0xDF3979) instead of at 0xFFE0F7.
        // if (e) ((void (*)(DWORD64))(g_base + 0xE557B0))(e + 8);
        g_puppet = e;
        g_spawnState = 3;
    }
    // (v10's spin hack - PerformAttack on the puppet's attack component - is gone: the spin now comes with the
    // friend's behavior events like every other animation.)
    return EXCEPTION_CONTINUE_EXECUTION;
}

static volatile DWORD g_lastRxTick;
static unsigned g_session; static int g_shown = -1; static volatile LONG g_teleports;

// Crash diagnostics: the friend's game died during a Crash 3 level change without any Windows report. This handler
// (registered last) writes access violations / illegal instructions to our log with the faulting RVA and the first
// return addresses on the stack, then lets the game continue its normal handling.
static FILE *g_log;
static volatile LONG g_faults;
static LONG CALLBACK crash_logger(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_faults) > 5 || !g_log) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord; HMODULE me = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)crash_logger, &me);
    fprintf(g_log, "FAULT 0x%08lX at exe+0x%llX (our dll+0x%llX) addr 0x%llX thread %lu stack exe RVAs:", code,
        c->Rip - g_base, c->Rip - (DWORD64)me, (DWORD64)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    DWORD64 *sp = (DWORD64 *)c->Rsp, top = (DWORD64)((NT_TIB *)NtCurrentTeb())->StackBase; int n = 0;
    for (int k = 0; k < 256 && n < 10 && (DWORD64)(sp + k) < top; k++) { DWORD64 v = sp[k]; if (v - g_base < 0x1D74000) { fprintf(g_log, " 0x%llX", v - g_base); n++; } }
    fprintf(g_log, "\n"); fflush(g_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void read_input(DWORD64 ai, unsigned char *o) {
    memset(o, 0, 48); if (!ai) return;
    for (int i = 0, k = 0; i < 6; k += IN_LEN[i], i++) safe_read(ai + IN_OFF[i], o + k, IN_LEN[i]);
}
// Buttons (ActorInput +0x20, "now"): forward 0x4000, jump 0x01000000, spin 0x02000000 (~80 ms), crouch/slide
// 0x00400000. The game detects a press by comparing "now" with its own "previous" (+0x78): copying the sender's
// +0x78 too made every press look already held, so spins never fired. Index 1 (+0x78) is no longer written, and every
// received button bit is held for at least 60 ms so a short press is seen by the puppet's update.
// Both 32-bit words are latched: +0x20 (held buttons) and +0x24 (press flags; a spin also sets 0x10000000 there,
// a crouch 0x02040000, a jump 0x24000000). Bit 0x60 of +0x24 is always set and stays as received.
static DWORD g_bitSeen[64];
static void write_input(DWORD64 ai, const unsigned char *in) {
    if (!ai) return;
    unsigned long long now = 0; memcpy(&now, in, 8); DWORD t = GetTickCount();
    for (int b = 0; b < 64; b++) { if (now & (1ull << b)) g_bitSeen[b] = t; else if (g_bitSeen[b] && t - g_bitSeen[b] < 60) now |= 1ull << b; }
    unsigned char tmp[48]; memcpy(tmp, in, 48); memcpy(tmp, &now, 8);
    for (int i = 0, k = 0; i < 6; k += IN_LEN[i], i++) if (i != 1) safe_write(ai + IN_OFF[i], tmp + k, IN_LEN[i]);
}
// sender side: OR of the button words seen since the last packet, so presses shorter than the send interval survive
static unsigned long long g_pendingNow;
static void take_packet(const Packet *p) {
    if (p->magic != MAGIC || p->version != PKT_VERSION) return;
    EnterCriticalSection(&g_cs);
    if (!g_peerValid || p->session != g_peerPkt.session || (int)(p->seq - g_lastSeq) > 0) {
        static unsigned lastEv, evSession;
        if (p->session != evSession) { evSession = p->session; lastEv = p->evSeq; }   // new friend session: no backlog
        // evId[k] is the friend's event number evSeq - NEV + k: queue the ones not seen yet, in order
        unsigned first = p->evSeq - NEV; if ((int)(lastEv - first) > 0) first = lastEv;
        for (unsigned s = first; (int)(p->evSeq - s) > 0; s++) {
            short id = p->evId[s - (p->evSeq - NEV)];
            if (id >= 0 && g_pupBC && g_rqHead - g_rqTail < 240) { g_rq[g_rqHead & 255] = id; g_rqHead++; }
        }
        lastEv = p->evSeq;
        {   // crates the friend broke: crPos[k] is break number crSeq - NCR + k
            static unsigned lastCr, crSession;
            if (p->session != crSession) { crSession = p->session; lastCr = p->crSeq; }
            unsigned f = p->crSeq - NCR; if ((int)(lastCr - f) > 0) f = lastCr;
            for (unsigned s = f; (int)(p->crSeq - s) > 0; s++)
                if (g_inCrHead - g_inCrTail < 60) { LONG h = g_inCrHead; memcpy(g_inCr[h & 63], p->crPos[s - (p->crSeq - NCR)], 12); g_inCrLevel[h & 63] = p->level; g_inCrHead = h + 1; }
            lastCr = p->crSeq;
        }
        g_peerFruit = p->fruitTotal; g_peerFruitValid = 1;
        g_peerRunSpeed = p->runSpeed; g_peerTurbo = p->turbo; g_peerGround = p->groundState;
        g_peerPkt = *p; g_lastSeq = p->seq; g_peerValid = 1; g_lastRxTick = GetTickCount();
    }
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_rx);
}

// current level: [[exe+0x1A64690] + 0xB8] = char* (CClient), e.g. "crash1/l101_nsanitybeach/l101_nsanitybeach"
static unsigned level_hash(char *out, int cap) {
    DWORD64 client = rd(g_base + 0x1A64690), str = client ? rd(client + 0xB8) : 0;
    char s[128] = {0};
    if (!str || !safe_read(str, s, sizeof s - 1)) { if (out) out[0] = 0; return 0; }
    s[sizeof s - 1] = 0;
    unsigned h = 2166136261u;
    for (char *c = s; *c; c++) { char ch = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c; h = (h ^ (unsigned char)ch) * 16777619u; }
    if (out) { strncpy(out, s, cap - 1); out[cap - 1] = 0; }
    return h;
}
static DWORD WINAPI udp_rx(LPVOID arg) {
    (void)arg; Packet p;
    while (!stop_requested()) { int n = recv(g_sock, (char *)&p, sizeof p, 0); if (n == (int)sizeof p) take_packet(&p); }
    return 0;
}
static void steam_poll(void) {
    unsigned size; Packet p; unsigned long long from;
    while (S.avail(S.net, &size, STEAM_CHAN)) {
        if (!S.read(S.net, &p, sizeof p, &size, &from, STEAM_CHAN)) break;
        if (from != S.peer) { InterlockedIncrement(&g_rxRejected); continue; }   // only the configured friend
        if (size == sizeof p) take_packet(&p);
    }
}
static void send_state(const Packet *p) {
    if (g_steam) S.send(S.net, S.peer, p, sizeof *p, 1 /* UnreliableNoDelay */, STEAM_CHAN);
    else sendto(g_sock, (const char *)p, sizeof *p, 0, (struct sockaddr *)&g_peerAddr, sizeof g_peerAddr);
}

static int steam_init(void) {
    HMODULE m = GetModuleHandleA("steam_api64.dll");
    if (!m) { fprintf(g_log, "steam_api64.dll not loaded\n"); return 0; }
    CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(m, "SteamInternal_CreateInterface");
    GetHandleFn hu = (GetHandleFn)GetProcAddress(m, "SteamAPI_GetHSteamUser"), hp = (GetHandleFn)GetProcAddress(m, "SteamAPI_GetHSteamPipe");
    GetIfaceFn gn = (GetIfaceFn)GetProcAddress(m, "SteamAPI_ISteamClient_GetISteamNetworking");
    GetIfaceFn gu = (GetIfaceFn)GetProcAddress(m, "SteamAPI_ISteamClient_GetISteamUser");
    GetSteamIDFn gid = (GetSteamIDFn)GetProcAddress(m, "SteamAPI_ISteamUser_GetSteamID");
    S.send = (SendP2PFn)GetProcAddress(m, "SteamAPI_ISteamNetworking_SendP2PPacket");
    S.avail = (AvailP2PFn)GetProcAddress(m, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    S.read = (ReadP2PFn)GetProcAddress(m, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    S.accept = (AcceptP2PFn)GetProcAddress(m, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    RelayFn relay = (RelayFn)GetProcAddress(m, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
    if (!ci || !hu || !hp || !gn || !gu || !gid || !S.send || !S.avail || !S.read || !S.accept || !relay) { fprintf(g_log, "missing steam export\n"); return 0; }
    void *client = ci("SteamClient017"); int user = hu(), pipe = hp();
    S.net = client ? gn(client, user, pipe, "SteamNetworking005") : NULL;
    S.user = client ? gu(client, user, pipe, "SteamUser019") : NULL;
    S.me = S.user ? gid(S.user) : 0;
    fprintf(g_log, "steam: client %p hUser %d hPipe %d networking %p user %p my SteamID %llu\n", client, user, pipe, S.net, S.user, S.me);
    if (!S.net || !S.me) return 0;
    relay(S.net, TRUE);
    return 1;
}

// --- shared wumpa counter, run from the worker loop every ~50 ms -------------------------------------------------------
static unsigned g_myFruit;                  /* fruit we gained ourselves since start (sent) */
static DWORD64 g_hud, g_hudFor;             /* player's HUD script component, and the player it belongs to */
static DWORD64 g_wc; static int g_wcPrev = -1, g_hudPrev = -1;   /* locked real counter, last values seen */
static DWORD64 g_cand[MAXGV]; static int g_candN = -1;           /* -1 = not started */
static unsigned g_peerFruitSeen; static int g_peerFruitInit; static int g_fruitPending;
// A crate we break for the friend gives OUR player its fruit when we are close. Those fruit are the friend's (they send
// them anyway): for 4 s after such a hit our gains are not sent and are taken off the friend's incoming fruit
// (g_fruitPending may go negative), so nothing is counted twice and nothing echoes back.
static volatile DWORD g_lastRemoteHit;
static void count_own(int d) {
    if (!d) return;
    if (g_lastRemoteHit && GetTickCount() - g_lastRemoteHit < 4000) g_fruitPending -= d; else g_myFruit += d;
}
static int rd32(DWORD64 a, int *v) { return safe_read(a, v, 4); }
static DWORD64 find_hud(DWORD64 actor) {
    DWORD64 cl = actor ? rd(actor + 0x18) : 0, arr = cl ? rd(cl + 0x20) : 0; int n = 0;
    if (cl) safe_read(cl + 0x10, &n, 4);
    for (int i = 0; i < n && i < 200 && arr; i++) {
        DWORD64 o = rd(arr + 8 * i); char nm[33] = {0};
        if (!o || rd(o) != g_base + VSC_VT) continue;
        DWORD64 info = rd(o + 0x38), s = info ? rd(info + 0x10) : 0;
        if (s && safe_read(s, nm, 32) && !memcmp(nm, "Scripts.Graph.CrashBandicoot_HUD", 33)) return o;
    }
    return 0;
}
static int own_gain(int prev, int now) {   // fruit gained between two counter readings (100 fruit = 1 life, back to 0)
    int d = now - prev;
    if (d < 0 && prev >= 80 && now <= 30) d += 100;
    return d > 0 && d < 60 ? d : 0;
}
static void fruit_tick(DWORD64 actor) {
    if (actor != g_hudFor) { g_hudFor = actor; g_hud = find_hud(actor); g_wc = 0; g_wcPrev = -1; g_hudPrev = -1; g_candN = -1;
        fprintf(g_log, "wumpa: HUD script %s\n", g_hud ? "found" : "not found (no shared fruit here)"); fflush(g_log); }
    if (!actor || !g_hud) return;
    int hud = -1; DWORD64 vt = 0;
    if (!safe_read(g_hud, &vt, 8) || vt != g_base + VSC_VT || !rd32(g_hud + 0x80, &hud)) { g_hudFor = 0; return; }
    if (g_wc) {   // locked: the real counter
        int w = -1;
        if (!safe_read(g_wc, &vt, 8) || vt != g_base + GVV_VT || !rd32(g_wc + 0x10, &w)) { g_wc = 0; g_candN = -1; return; }
        if (g_wcPrev >= 0) count_own(own_gain(g_wcPrev, w));
        g_wcPrev = w;
        if (g_fruitPending > 0) {
            int nw = w + g_fruitPending; if (nw > 99) nw = 99;     // the game itself turns 100 into a life on the next fruit
            if (nw != w) safe_write(g_wc + 0x10, &nw, 4);
            fprintf(g_log, "wumpa: +%d from the friend (%d -> %d)\n", g_fruitPending, w, nw); fflush(g_log);
            g_fruitPending = 0; g_wcPrev = nw;
        }
    } else {      // not locked yet: own gains from the HUD copy, and narrow the candidates when it changes
        if (g_hudPrev >= 0 && hud != g_hudPrev) count_own(own_gain(g_hudPrev, hud));
        if (g_candN < 0 || hud != g_hudPrev) {
            EnterCriticalSection(&g_crCs);
            if (g_candN < 0) { if (g_gvN) { memcpy(g_cand, g_gv, g_gvN * sizeof g_gv[0]); g_candN = g_gvN; } }
            int k = 0;
            for (int i = 0; i < g_candN; i++) { int v = -1; DWORD64 cv = 0;
                if (safe_read(g_cand[i], &cv, 8) && cv == g_base + GVV_VT && rd32(g_cand[i] + 0x10, &v) && v == hud) g_cand[k++] = g_cand[i]; }
            if (g_candN >= 0) g_candN = k;
            LeaveCriticalSection(&g_crCs);
            if (g_candN == 1 && hud > 0) { g_wc = g_cand[0]; g_wcPrev = hud; fprintf(g_log, "wumpa: counter found (%d)\n", hud); fflush(g_log); }
            else if (g_candN == 0) g_candN = -1;   // lost it (rescan, level change): start over
        }
        g_hudPrev = hud;
    }
    // the friend's fruit: what their total gained since we last looked (first packet of a session = baseline only)
    if (g_peerFruitValid) {
        unsigned t = g_peerFruit;
        if (!g_peerFruitInit || t < g_peerFruitSeen) { g_peerFruitSeen = t; g_peerFruitInit = 1; }
        else if (t != g_peerFruitSeen) { int d = (int)(t - g_peerFruitSeen); if (d > 0 && d < 500) g_fruitPending += d; g_peerFruitSeen = t; }
    }
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    char path[MAX_PATH], name[16], mode[16], a1[64] = "", a3[64] = ""; int a2 = 0, a4 = 0;
    sprintf(path, "%scoop-%lu.cfg", g_dir, GetCurrentProcessId());
    FILE *cfg = fopen(path, "r"); if (!cfg) return 1;
    int n = fscanf(cfg, "%15s %15s %63s %d %63s %d", name, mode, a1, &a2, a3, &a4);
    fclose(cfg); if (n < 2) return 1;
    sprintf(path, "%scoop-%s.log", g_dir, name);
    g_log = fopen(path, "w"); if (!g_log) return 1;
    g_base = (DWORD64)GetModuleHandleA(NULL);
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_crCs);
    // Without this, Sleep(2) lasts ~15.6 ms (default timer resolution): the loop ran 8x slower than designed and sent
    // only ~8 packets/s instead of ~60.
    timeBeginPeriod(1);
    AddVectoredExceptionHandler(0, crash_logger);   // last in line: logs fatal-looking faults, never handles them
    g_session = (GetTickCount() * 2654435761u) ^ (GetCurrentProcessId() << 16) ^ (unsigned)(DWORD64)&g_cs;
    { char cp[MAX_PATH]; sprintf(cp, "%scoop.coco", g_dir); g_wantCoco = GetFileAttributesA(cp) != INVALID_FILE_ATTRIBUTES; }
    { char lv[128]; level_hash(lv, sizeof lv); fprintf(g_log, "mod v15 (packet v8)%s, session %08X, level now '%s'\n", g_wantCoco ? ", friend shown as Coco" : "", g_session, lv); fflush(g_log); }

    DWORD rxTid = 0;
    if (!strcmp(mode, "steam") || !strcmp(mode, "steamprobe")) {
        g_steam = 1;
        if (!steam_init()) { fprintf(g_log, "steam init failed\n"); fclose(g_log); return 1; }
        if (!strcmp(mode, "steamprobe")) { fprintf(g_log, "probe only: Steam P2P interfaces OK\n"); fclose(g_log); return 0; }
        S.peer = _strtoui64(a1, NULL, 10);
        fprintf(g_log, "steam peer %llu\n", S.peer);
        if (!S.peer || S.peer == S.me) { fprintf(g_log, "bad peer SteamID (empty or my own)\n"); fclose(g_log); return 1; }
        S.accept(S.net, S.peer);
    } else {
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in local = {0};
        local.sin_family = AF_INET; local.sin_port = htons((u_short)a2); inet_pton(AF_INET, a1, &local.sin_addr);
        g_peerAddr.sin_family = AF_INET; g_peerAddr.sin_port = htons((u_short)a4); inet_pton(AF_INET, a3, &g_peerAddr.sin_addr);
        DWORD tmo = 200; setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof tmo);
        if (bind(g_sock, (struct sockaddr *)&local, sizeof local) != 0) { fprintf(g_log, "udp bind failed %d\n", WSAGetLastError()); fclose(g_log); return 1; }
        HANDLE rt = CreateThread(NULL, 0, udp_rx, NULL, 0, NULL); rxTid = GetThreadId(rt);
        fprintf(g_log, "udp %s:%d -> %s:%d\n", a1, a2, a3, a4);
    }
    fflush(g_log);
    PVOID vehHandle = AddVectoredExceptionHandler(1, veh);   // permanent: animation hooks + one-shot spawn hook
    g_animOn = 1; arm_all(rxTid); DWORD lastArm = GetTickCount();
    DWORD scanTid = 0; { HANDLE st = CreateThread(NULL, 0, crate_scanner, NULL, 0, NULL); if (st) { scanTid = GetThreadId(st); CloseHandle(st); } }
    DWORD lastScan = 0, lastCrPoll = 0; DWORD64 scannedFor = 0;

    // Level-change safety: the puppet belongs to the level it was spawned in. Whenever the player actor changes
    // (menu, map, new level, reload) the puppet is forgotten at once, and a new one is spawned only after the new
    // player has been stable for 2 s. Writing into a puppet from a previous level wrote into freed memory and
    // killed both test copies on the map -> level transition.
    DWORD64 actor = 0, lastActor = 0; DWORD stableSince = 0;
    unsigned seq = 0; LONG applied = 0; int tick = 0;
    while (!stop_requested()) {
        DWORD64 now = player_actor();
        // is the friend there (heard < 3 s ago) and in the same level as us?
        Packet cur; EnterCriticalSection(&g_cs); cur = g_peerPkt; LeaveCriticalSection(&g_cs);
        int show = g_peerValid && GetTickCount() - g_lastRxTick < 3000 && cur.level != 0 && cur.level == level_hash(NULL, 0);
        if (now != lastActor) { g_pupBC = 0; g_myBC = 0; g_rqTail = g_rqHead; }
        if (GetTickCount() - lastArm > 2000) { arm_all(rxTid); lastArm = GetTickCount(); }   // threads created since
        if (now != lastActor) {
            g_puppet = 0; g_proto = 0; g_spawnState = 0; lastActor = now; stableSince = GetTickCount();
            { char lv[128]; level_hash(lv, sizeof lv); fprintf(g_log, "player changed -> 0x%llX (puppet dropped), level '%s'\n", now, lv); fflush(g_log); }
        } else if (now && show && !g_puppet && g_spawnState == 0 && GetTickCount() - stableSince > 2000 && !strstr(name, "nospawn")) {
            // spawn only when the friend is actually visible (a hidden puppet is parked below the level, where the
            // game's kill plane destroys it cleanly; it is respawned here when the friend comes back)
            // which hero: the one the friend plays (packet v8); "Show my friend as Coco" turns a Crash friend into Coco
            char mine[8], want[8]; my_hero(mine);
            strcpy(want, cur.hero[0] ? cur.hero : mine); want[7] = 0;
            if (g_wantCoco && !strcmp(want, "Crash")) strcpy(want, "Coco");
            if (strcmp(want, "Crash") && strcmp(want, "Coco")) strcpy(want, mine);   // unknown name: stay safe
            g_proto = 0;
            if (strcmp(want, mine)) {   // not our own hero: its pack is not loaded, ask for it once per level and hero
                static DWORD64 packFor; static char packName[8];
                if (packFor != now || strcmp(packName, want)) {
                    packFor = now; strcpy(packName, want); strcpy(g_packName, want);
                    g_cocoReq = 1; for (int i = 0; i < 100 && g_cocoReq != 2; i++) Sleep(10);
                    fprintf(g_log, "hero: %s pack requested (request 0x%llX)\n", want, g_cocoReqObj); fflush(g_log);
                }
                strcpy(g_protoName, want);
                for (int i = 0; i < 16 && !(g_proto = find_prototype()); i++) Sleep(500);
                if (!g_proto) fprintf(g_log, "hero: no %s prototype, using %s\n", want, mine);
            }
            if (!g_proto) { strcpy(g_protoName, mine); g_proto = find_prototype(); }
            if (g_proto && player_actor() == now && safe_read(now + 0x20, g_spawnPos, 12)) {
                g_spawnPos[1] += 150.0f;
                g_poolOwner = now;
                g_spawnState = 1; set_all(g_base + HOOK_RVA, 1ull, rxTid);
                for (int i = 0; i < 100 && g_spawnState != 3; i++) Sleep(50);
                set_all(0, 0, rxTid); Sleep(200);
                if (player_actor() != now) g_puppet = 0;      // level changed during the spawn
                strcpy(g_pupHero, g_protoName); strcpy(g_pupFor, cur.hero);
                // events only when the puppet has the same hero (same behavior graph) as the friend
                g_pupCoco = strcmp(g_pupHero, cur.hero[0] ? cur.hero : "Crash") != 0;
            } else g_spawnState = 4;                        // no prototype in this level: don't retry until it changes
            fprintf(g_log, "player 0x%llX prototype 0x%llX spawn state %ld puppet 0x%llX\n", now, g_proto, g_spawnState, g_puppet); fflush(g_log);
        }
        {   // crates: rescan, detect our own breaks, replay the friend's
            DWORD tn = GetTickCount();
            if (now != scannedFor) {
                EnterCriticalSection(&g_crCs); g_crN = 0; g_bounceDef = 0; LeaveCriticalSection(&g_crCs);
                scannedFor = now; lastScan = 0; g_inCrTail = g_inCrHead; g_hitReq = 0;
            }
            if (now && !g_scanReq && tn - stableSince > 3000 && (lastScan == 0 || tn - lastScan > 15000)) { g_scanFor = now; g_scanReq = 1; lastScan = tn; }
            if (now && tn - lastCrPoll > 100) {
                lastCrPoll = tn;
                static int gone[MAXCR]; int ng = 0;
                EnterCriticalSection(&g_crCs);
                for (int i = 0; i < g_crN; i++) { DWORD64 vt = 0; if (!safe_read(g_crComp[i], &vt, 8) || vt != g_base + DESTR_VT) gone[ng++] = i; }
                if (ng > 0 && ng <= 3) {
                    // only crates near our player count as broken by us: far ones vanish when the game unloads that part
                    // of the level (seen in the lab: a crate 6000 units away "broke"), and the same spot is sent once
                    float me[3] = {0}; safe_read(now + 0x20, me, 12);
                    static float sentP[32][3]; static int sentN;
                    for (int k = 0; k < ng; k++) {
                        float *c = g_crP[gone[k]], dx = c[0] - me[0], dy = c[1] - me[1], dz = c[2] - me[2]; int dup = 0;
                        for (int s = 0; s < sentN && s < 32; s++) { float ex = sentP[s][0] - c[0], ey = sentP[s][1] - c[1], ez = sentP[s][2] - c[2]; if (ex * ex + ey * ey + ez * ez < 25.0f) dup = 1; }
                        if (dup) continue;
                        if (dx * dx + dy * dy + dz * dz > 600.0f * 600.0f) { fprintf(g_log, "crate gone far away at %.1f %.1f %.1f (unloaded?), not sent\n", c[0], c[1], c[2]); continue; }
                        memcpy(sentP[sentN++ & 31], c, 12);
                        LONG h = g_myCrHead; memcpy(g_myCr[h & 255], c, 12); g_myCrHead = h + 1; g_crSent++;
                        fprintf(g_log, "crate broken here at %.1f %.1f %.1f (sent)\n", c[0], c[1], c[2]); }
                } else if (ng > 3) {   // many at once = the game rebuilt them (death / checkpoint): not real breaks
                    fprintf(g_log, "%d crates vanished at once (level reload?), not sent, rescanning\n", ng); lastScan = tn - 14000;
                }
                for (int k = ng - 1; k >= 0; k--) { int i = gone[k]; g_crN--; g_crComp[i] = g_crComp[g_crN]; memcpy(g_crP[i], g_crP[g_crN], 12); }
                if (ng) fflush(g_log);
                LeaveCriticalSection(&g_crCs);
            }
            { static DWORD lastFruit; if (tn - lastFruit > 50) { lastFruit = tn; fruit_tick(now); } }
            if (g_hitReq == 2) g_hitReq = 0;
            if (now && g_hitReq == 0 && g_inCrTail != g_inCrHead) {
                LONG t = g_inCrTail; float q[3]; memcpy(q, g_inCr[t & 63], 12);
                if (g_inCrLevel[t & 63] != level_hash(NULL, 0)) g_inCrTail = t + 1;                  // friend's other level
                else if ((!g_bounceDef || !g_crN) && tn - stableSince < 20000) { /* our scan is not done yet: wait */ }
                else {
                    int best = -1; float bd = 25.0f * 25.0f;
                    EnterCriticalSection(&g_crCs);
                    for (int i = 0; i < g_crN; i++) { float dx = g_crP[i][0] - q[0], dy = g_crP[i][1] - q[1], dz = g_crP[i][2] - q[2], d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; best = i; } }
                    if (best >= 0 && g_bounceDef) {
                        g_hitComp = g_crComp[best]; g_hitTgt = rd(g_hitComp + 0x18); g_hitDef = g_bounceDef; g_hitAtt = now;
                        g_crN--; g_crComp[best] = g_crComp[g_crN]; memcpy(g_crP[best], g_crP[g_crN], 12);   // not "our" break
                        g_hitReq = 1; g_crRemote++; g_lastRemoteHit = GetTickCount();
                        fprintf(g_log, "friend broke the crate at %.1f %.1f %.1f: breaking ours\n", q[0], q[1], q[2]);
                    } else { g_crMiss++; fprintf(g_log, "friend broke a crate at %.1f %.1f %.1f: none here (already broken?)\n", q[0], q[1], q[2]); }
                    LeaveCriticalSection(&g_crCs); fflush(g_log);
                    g_inCrTail = t + 1;
                }
            }
        }
        if (g_steam) steam_poll();
        { DWORD64 a = player_actor(), ai = a ? rd(a + 0x1A8) : 0; unsigned long long n = 0; if (ai && safe_read(ai + 0x20, &n, 8)) g_pendingNow |= n; }
        if (tick % 8 == 0) {
            actor = player_actor();
            DWORD64 xf = actor ? rd(actor + 0x30) : 0;
            if (actor && xf) {
                Packet p; memset(&p, 0, sizeof p); p.magic = MAGIC; p.version = PKT_VERSION; read_input(rd(actor + 0x1A8), p.input); p.seq = ++seq; p.session = g_session; p.level = level_hash(NULL, 0);
                {   // animation: the last NEV events fired on our Crash + our run speed / turbo graph variables
                    LONG head = g_capHead; p.evSeq = (unsigned)head;
                    for (int k = 0; k < NEV; k++) { LONG s = head - NEV + k; p.evId[k] = s >= 0 ? g_cap[s & 255] : -1; }
                    DWORD64 bc = g_myBC, w = bc ? rd(rd(rd(rd(bc + 0x30) + 0x80) + 0x110) + 0x10) : 0;
                    if (w) { safe_read(w + 21 * 4, &p.runSpeed, 4); safe_read(w + 18 * 4, &p.turbo, 4); safe_read(w + 24 * 4, &p.groundState, 4); }
                }
                p.fruitTotal = g_myFruit; my_hero(p.hero);
                { LONG h = g_myCrHead; p.crSeq = (unsigned)h;
                  for (int k = 0; k < NCR; k++) { LONG s = h - NCR + k; if (s >= 0) memcpy(p.crPos[k], g_myCr[s & 255], 12); } }
                { unsigned long long now; memcpy(&now, p.input, 8); now |= g_pendingNow; memcpy(p.input, &now, 8); g_pendingNow = 0; }
                if (safe_read(actor + 0x20, p.pos, 12) && safe_read(xf + 0x60, p.euler, 12) && safe_read(xf + 0x10, p.quat, 16))
                    send_state(&p);
            }
            if (g_steam && tick % 500 == 0) S.accept(S.net, S.peer);
        }
        DWORD64 pup = g_puppet, pxf = pup ? rd(pup + 0x30) : 0, pvt = 0;
        if (pup && (!safe_read(pup, &pvt, 8) || pvt != g_base + CACTOR_VT || player_actor() != lastActor)) {
            g_puppet = 0; pup = 0; g_spawnState = 0; stableSince = GetTickCount();   // destroyed by the game: allow a respawn
            fprintf(g_log, "puppet gone (destroyed by the game), will respawn when the friend is visible\n"); fflush(g_log);
        }
        // the friend switched hero: retire this puppet (kept parked above the level, never left to fall into the kill
        // plane) and spawn one with the new hero
        static DWORD64 retired[8]; static int nRetired;
        if (pup && show && cur.hero[0] && strcmp(cur.hero, g_pupFor) && nRetired < 8) {
            fprintf(g_log, "friend switched hero %s -> %s: new puppet\n", g_pupFor, cur.hero); fflush(g_log);
            retired[nRetired++] = pup; g_puppet = 0; pup = 0; pxf = 0; g_spawnState = 0; stableSince = GetTickCount() - 1500;
        }
        if (now != lastActor) nRetired = 0;
        for (int r = 0; r < nRetired; r++) { DWORD64 rv = 0; DWORD fl = 0; float hi[3] = { 0, 0, 100000.0f };
            if (!safe_read(retired[r], &rv, 8) || rv != g_base + CACTOR_VT) continue;
            safe_write(retired[r] + 0x20, hi, 12); if (safe_read(retired[r] + 0x38, &fl, 4)) { fl |= 0x400; safe_write(retired[r] + 0x38, &fl, 4); } }
        {   // behavior characters: looking them up walks ~75 components, so only every 250 ms or when an actor changes
            // (doing it every loop slowed the loop 4x: ~17 packets/s instead of ~60)
            static DWORD64 lastMe, lastPup; static DWORD lastLook;
            DWORD64 wantPup = (pup && show) ? pup : 0;
            if (now != lastMe || wantPup != lastPup || GetTickCount() - lastLook > 250) {
                g_myBC = behavior_char(now); g_pupBC = wantPup ? behavior_char(wantPup) : 0;
                lastMe = now; lastPup = wantPup; lastLook = GetTickCount();
            }
        }
        static DWORD lastNw; static int nowrite;
        if (GetTickCount() - lastNw > 250) { char np[MAX_PATH]; sprintf(np, "%scoop.nowrite", g_dir); nowrite = GetFileAttributesA(np) != INVALID_FILE_ATTRIBUTES;
            sprintf(np, "%scoop.noevents", g_dir); g_noEvents = GetFileAttributesA(np) != INVALID_FILE_ATTRIBUTES; lastNw = GetTickCount(); }
        {   // log every replayed event id (diagnostics)
            static LONG logged; LONG r = g_replayed;
            if (r - logged > 200) logged = r - 200;
            if (r != logged) { fprintf(g_log, "replayed:"); for (; logged < r; logged++) fprintf(g_log, " %d", g_replayLog[logged & 255]); fprintf(g_log, "\n"); fflush(g_log); }
        }
        if (pup && !nowrite) {   /* pxf may be 0 (no transform yet): position-only fallback below */
            Packet q = cur;
            // friend absent: park the puppet far ABOVE the level. Parking it below (v11) made the game's kill plane
            // destroy it; in a real game (2026-11-02) a second such destruction crashed the game at exe+0xDDE1D0
            // (null component during the entity teardown, stack 0xE01AAB 0xDF3998 0xDDDE4B).
            if (!show) { q.pos[0] = 0; q.pos[1] = 0; q.pos[2] = 100000.0f; memset(q.input, 0, sizeof q.input); }
            write_input(rd(pup + 0x1A8), q.input);   // the friend's buttons + stick (the run cycle needs the stick)
            if (show != g_shown) { fprintf(g_log, "friend %s\n", show ? "visible" : "hidden (absent or other level)"); fflush(g_log); g_shown = show; }
            DWORD flags = 0; BYTE one = 1;
            // Position is authoritative (written every frame, as in v6/v10). Velocity steering (v7-v9) fell apart over
            // the internet. The animation comes from the friend's behavior events (see the VEH), not from velocity.
            if (safe_write(pup + 0x20, q.pos, 12) && safe_read(pup + 0x38, &flags, 4)) { flags |= 0x400; safe_write(pup + 0x38, &flags, 4); }
            if (pxf && safe_write(pxf + 0x60, q.euler, 12) && safe_write(pxf + 0x10, q.quat, 16) && safe_write(pxf + 0x7C, &one, 1)
                && safe_read(pup + 0x38, &flags, 4)) {
                flags |= 0x800; safe_write(pup + 0x38, &flags, 4);
            }
            applied++;
        }
        if (++tick % 500 == 0) {
            fprintf(g_log, "t=%ds sent %u received %ld rejected %ld peer seq %u pos %.1f %.1f %.1f yaw %.2f applied %ld events captured %ld replayed %ld forced %ld crates known %d sent %ld replayed %ld missed %ld fruit mine %u friend %u counter %s\n", tick / 500,
                seq, g_rx, g_rxRejected, g_lastSeq, g_peerPkt.pos[0], g_peerPkt.pos[1], g_peerPkt.pos[2], g_peerPkt.euler[2], applied, g_captured, g_replayed, g_forced, g_crN, g_crSent, g_crRemote, g_crMiss, g_myFruit, g_peerFruit, g_wc ? "found" : "searching");
            fflush(g_log);
        }
        Sleep(2);
    }
    // on stop: park our puppet below the level for 1 s so the game's kill plane removes it (nothing left frozen)
    for (int i = 0; i < 500 && g_puppet; i++) {
        DWORD64 pup = g_puppet, pvt = 0;
        if (!safe_read(pup, &pvt, 8) || pvt != g_base + CACTOR_VT) break;
        float down[3] = { 0, 0, -100000.0f }; DWORD flags = 0;
        safe_write(pup + 0x20, down, 12);
        if (safe_read(pup + 0x38, &flags, 4)) { flags |= 0x400; safe_write(pup + 0x38, &flags, 4); }
        Sleep(2);
    }
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); WSACleanup(); }
    timeEndPeriod(1);
    g_pupBC = 0; g_animOn = 0; arm_all(0); Sleep(100); RemoveVectoredExceptionHandler(vehHandle);
    fprintf(g_log, "stopped\n"); fclose(g_log);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res) {
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH); { char *s = strrchr(g_dir, 0x5C); if (s) s[1] = 0; }
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
