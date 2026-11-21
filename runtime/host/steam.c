// Steam bridge: the game's steam_api calls go to the real Linux Steam client (~/.steam/sdk64/steamclient.so),
// the same idea as Proton's lsteamclient. Ownership/login are checked by Steam itself; nothing here fakes them.
//
// Interfaces: each native 64-bit interface gets a guest wrapper object (vtable of stub addresses). A method index
// means the same method in the 32-bit guest vtable and the 64-bit native one (same interface version), so one
// generic forwarder converts arguments per a small spec table and calls native vtable[idx].
#define _GNU_SOURCE
#include "host.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#define G_STEAM 0xFFE00000u            // stub (wrapper w, method i) = G_STEAM + 16 * (64 * w + i)
#define MAX_WRAP 64
#define STEAM "steam_api.dll"

typedef struct { void *native; const char *ver; uint32_t gobj; } Wrap;
static Wrap wraps[MAX_WRAP];
static uint32_t n_wraps;
static __thread uint32_t cur_stub;     // which stub the current call came through (set by steam_resolve)

static void *lib, *client;
static int32_t pipe_h, user_h;
static void *(*CreateInterface)(const char *, int *);
static uint8_t (*BGetCallback)(int32_t, void *);
static void (*FreeLastCallback)(int32_t);
static uint8_t (*GetAPICallResult)(int32_t, uint64_t, void *, int, int, uint8_t *);

// returns RAX:RDX, so small structs the native ABI returns in two registers come back whole
typedef unsigned __int128 (*Native)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define VT(obj, i) ((*(Native **)(obj))[i])

// args: u = 32-bit value, q = 64-bit value (2 slots), p = pointer/string, I = interface version string
//       ! (first) = don't forward, just return 0
// ret:  v void, u 32-bit, b bool, q 64-bit, S 64-bit via MSVC hidden return pointer, i interface (wrap, version from I),
//       s const char* (copied into guest memory). MSVC returns every struct from a method via a hidden pointer;
//       the native ABI uses registers: S = 8-byte struct, D = 2-byte (digital action data), A = 13-byte (analog)
typedef struct { const char *iface; int idx; const char *args; char ret; const char *name; } Spec;
static const Spec specs[] = {
    {"SteamClient017", 5, "uuI", 'i', "GetISteamUser"},
    {"SteamClient017", 8, "uuI", 'i', "GetISteamFriends"},
    {"SteamClient017", 9, "uI", 'i', "GetISteamUtils"},
    {"SteamClient017", 10, "uuI", 'i', "GetISteamMatchmaking"},
    {"SteamClient017", 11, "uuI", 'i', "GetISteamMatchmakingServers"},
    {"SteamClient017", 12, "uuI", 'i', "GetISteamGenericInterface"},
    {"SteamClient017", 13, "uuI", 'i', "GetISteamUserStats"},
    {"SteamClient017", 15, "uuI", 'i', "GetISteamApps"},
    {"SteamClient017", 16, "uuI", 'i', "GetISteamNetworking"},
    {"SteamClient017", 17, "uuI", 'i', "GetISteamRemoteStorage"},
    {"SteamClient017", 18, "uuI", 'i', "GetISteamScreenshots"},
    {"SteamClient017", 21, "!u", 'v', "SetWarningMessageHook"},   // guest function pointer: keep Steam's default
    {"SteamClient017", 23, "uuI", 'i', "GetISteamHTTP"},
    {"SteamClient017", 25, "uuI", 'i', "GetISteamController"},
    {"SteamClient017", 26, "uuI", 'i', "GetISteamUGC"},
    {"SteamClient017", 27, "uuI", 'i', "GetISteamAppList"},
    {"SteamClient017", 28, "uuI", 'i', "GetISteamMusic"},
    {"SteamClient017", 29, "uuI", 'i', "GetISteamMusicRemote"},
    {"SteamClient017", 30, "uuI", 'i', "GetISteamHTMLSurface"},
    {"SteamClient017", 34, "uuI", 'i', "GetISteamInventory"},
    {"SteamClient017", 35, "uuI", 'i', "GetISteamVideo"},
    {"SteamClient017", 36, "uuI", 'i', "GetISteamParentalSettings"},
    {"SteamUser019", 0, "", 'u', "GetHSteamUser"},
    {"SteamUser019", 1, "", 'b', "BLoggedOn"},
    {"SteamUser019", 2, "", 'S', "GetSteamID"},
    {"SteamUtils009", 3, "", 'u', "GetServerRealTime"},   // Night 3 start, 0x6aa632 (export: vtable +0xc)
    {"SteamUtils009", 26, "", 'b', "IsSteamInBigPictureMode"},
    // indices from steam_api.dll's flat exports (42 GetUserRestrictions, 44 ClearRichPresence); call site 0x6aaf40
    {"SteamFriends015", 0, "", 's', "GetPersonaName"},   // found ahead (steam_calls.py): 0x6aa6d3 and 3 more
    {"SteamFriends015", 43, "pp", 'b', "SetRichPresence"},   // (key, value)
    {"SteamFriends015", 44, "", 'v', "ClearRichPresence"},
    {"SteamController006", 0, "", 'b', "Init"},
    {"SteamController006", 1, "", 'b', "Shutdown"},
    {"SteamController006", 2, "", 'v', "RunFrame"},
    // Steam Input poll, 0x7993f0 (per frame); layout checked against steam_api.dll's SteamAPI_ISteamController_* exports
    {"SteamController006", 3, "p", 'u', "GetConnectedControllers"},   // (ControllerHandle_t[16])
    {"SteamController006", 5, "p", 'q', "GetActionSetHandle"},
    {"SteamController006", 6, "qq", 'v', "ActivateActionSet"},
    {"SteamController006", 7, "q", 'q', "GetCurrentActionSet"},   // stage start, 0x79b712 (export: vtable +0x1c)
    {"SteamController006", 12, "p", 'q', "GetDigitalActionHandle"},
    {"SteamController006", 13, "qq", 'D', "GetDigitalActionData"},
    // (controller, action set, action, EControllerActionOrigin[8] out) -> count; HUD button glyphs, 0x79ba5c
    {"SteamController006", 14, "qqqp", 'u', "GetDigitalActionOrigins"},
    {"SteamController006", 15, "p", 'q', "GetAnalogActionHandle"},
    // same shape as #14, for the sticks: Config's Input tab (call site 0x79b8b3, vtable +0x44)
    {"SteamController006", 17, "qqqp", 'u', "GetAnalogActionOrigins"},
    {"SteamController006", 16, "qq", 'A', "GetAnalogActionData"},
    {"SteamController006", 21, "quu", 'v', "TriggerVibration"},   // (handle, left, right): only (h, 0, 0)
    {"SteamController006", 30, "q", 'u', "GetInputTypeForHandle"},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 0, "", 'b', "RequestCurrentStats"},
    // Found ahead of time by tools/recomp/steam_calls.py (every Steam call site in the exe); slots from steam_api.dll's
    // exports, argument shapes checked at the call sites. Achievements and leaderboards are real (the player's account).
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 7, "p", 'b', "SetAchievement"},   // 0x6aae61
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 10, "", 'b', "StoreStats"},       // 0x6aae73
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 22, "puu", 'q', "FindOrCreateLeaderboard"},   // (name, sort, display)
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 24, "q", 's', "GetLeaderboardName"},
    // (lb, ELeaderboardDataRequest, start, end): after a confession, 0x6ab324 (request 0 = global)
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 28, "quuu", 'q', "DownloadLeaderboardEntries"},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 29, "qpu", 'q', "DownloadLeaderboardEntriesForUsers"},   // (lb, CSteamID[], n)
    // (entries, index, LeaderboardEntry_t*, int32 details[], max): the entry is re-laid-out below (Linux packs 4)
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 30, "quppu", 'b', "GetDownloadedLeaderboardEntry"},   // 0x797776
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 31, "quupu", 'q', "UploadLeaderboardScore"},   // (lb, method, score, details[], n)
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 6, "pp", 'b', "GetAchievement"},   // (name, bool*); call site 0x6aaefd
    {"STEAMAPPS_INTERFACE_VERSION008", 0, "", 'b', "BIsSubscribed"},
    {"STEAMAPPS_INTERFACE_VERSION008", 1, "", 'b', "BIsLowViolence"},
    {"STEAMAPPS_INTERFACE_VERSION008", 2, "", 'b', "BIsCybercafe"},
    {"STEAMAPPS_INTERFACE_VERSION008", 3, "", 'b', "BIsVACBanned"},
    {"STEAMAPPS_INTERFACE_VERSION008", 4, "", 's', "GetCurrentGameLanguage"},
    {"STEAMAPPS_INTERFACE_VERSION008", 5, "", 's', "GetAvailableGameLanguages"},
    {"STEAMAPPS_INTERFACE_VERSION008", 6, "u", 'b', "BIsSubscribedApp"},
    {"STEAMAPPS_INTERFACE_VERSION008", 7, "u", 'b', "BIsDlcInstalled"},
};

static uint32_t wrap(void *native, const char *ver) {
    if (!native) return 0;
    for (uint32_t i = 0; i < n_wraps; i++) if (wraps[i].native == native) return wraps[i].gobj;
    if (n_wraps == MAX_WRAP) die("steam: too many interfaces");
    Wrap *w = &wraps[n_wraps];
    uint32_t vt = gmalloc(4 * 64);
    for (uint32_t i = 0; i < 64; i++) U32(vt + 4 * i) = G_STEAM + 16 * (64 * n_wraps + i);
    w->gobj = gmalloc(8);
    U32(w->gobj) = vt;
    U32(w->gobj + 4) = n_wraps;
    w->native = native;
    w->ver = strdup(ver);
    n_wraps++;
    return w->gobj;
}

static uint32_t guest_cstr(const char *s) {   // ponytail: each call leaks a copy; only language strings use this
    uint32_t p = gmalloc(strlen(s) + 1);
    strcpy(GSTR(p), s);
    return p;
}

// Scripted test presses (CATH_INPUT) reach Steam Input too: with a Steam controller the game reads its Steam Input
// actions (digital BTN_*, analog Move = left stick), not XInput. Digital handle -> the XInput button bit it stands for.
int steam_input_pads;
static struct { uint64_t handle; uint16_t bit; } actions[32];
static int n_actions;
static uint64_t move_handle = ~0ull;
static void action_handle(const char *name, uint64_t h, int analog) {
    static const struct { const char *name; uint16_t bit; } map[] = {{"BTN_A", 0x1000}, {"BTN_B", 0x2000},
        {"BTN_X", 0x4000}, {"BTN_Y", 0x8000}, {"BTN_START", 0x10}, {"BTN_BACK", 0x20}, {"BTN_LB", 0x100},
        {"BTN_RB", 0x200}, {"BTN_UP", 0x1}, {"BTN_DN", 0x2}, {"BTN_LT", 0x4}, {"BTN_RT", 0x8}};
    if (analog) { if (!strcmp(name, "Move")) move_handle = h; return; }
    for (int i = 0; i < n_actions; i++) if (actions[i].handle == h) return;   // asked again (per frame): known
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name) && n_actions < 32) actions[n_actions].handle = h, actions[n_actions++].bit = map[i].bit;
}
static void action_scripted(uint64_t h, unsigned __int128 *r) {   // ControllerDigitalActionData_t { bState, bActive }
    uint16_t s = input_scripted();
    for (int i = 0; i < n_actions; i++) {
        if (actions[i].handle != h) continue;
        if (s & actions[i].bit) ((uint8_t *)r)[0] = ((uint8_t *)r)[1] = 1;
    }
}
// input diagnosis: every digital action as Steam Input reports it, per (controller, action) and caller (first 400 changes)
static void log_digital(uint64_t pad, uint64_t h, unsigned __int128 r, uint32_t caller) {
    static struct { uint64_t pad, h; int v; } last[64];
    static int n, logged;
    int i, v = (int)(r & 0xFFFF), bit = 0;
    for (i = 0; i < n_actions; i++) if (actions[i].handle == h) bit = actions[i].bit;
    if (logged >= 400) return;
    for (i = 0; i < n && !(last[i].pad == pad && last[i].h == h); i++) {}
    if (i == n) { if (n == 64) return; last[n].pad = pad, last[n].h = h, last[n++].v = -1; }
    if (last[i].v == v) return;
    last[i].v = v;
    logged++;
    fprintf(stderr, "[input] steam %s pad %llx action %llx state %d active %d (from %08x) at %llu ms\n",
            bit == 0x1000 ? "A" : bit == 0x2000 ? "B" : bit == 0x10 ? "START" : "button", (unsigned long long)pad, (unsigned long long)h, v & 0xFF,
            v >> 8, caller, (unsigned long long)SDL_GetTicks());
}
// ControllerAnalogActionData_t { int32 eMode; float x, y; bool bActive }: the scripted D-pad pushes the Move stick fully
static void analog_scripted(uint64_t h, unsigned __int128 *r) {
    static int logged, held[64];   // input diagnosis: analog actions other than the sticks crossing 0.25 (first 100)
    float v;
    memcpy(&v, (uint8_t *)r + 4, 4);
    if (h > 2 && h < 64 && (v > .25f) != held[h] && logged++ < 100)
        held[h] = v > .25f, fprintf(stderr, "[input] steam analog action %llx %.2f at %llu ms\n", (unsigned long long)h, v,
                                    (unsigned long long)SDL_GetTicks());
    uint16_t s = input_scripted();
    if (h != move_handle || !(s & 0xF)) return;
    float x = (s & 8 ? 1.f : 0.f) - (s & 4 ? 1.f : 0.f), y = (s & 1 ? 1.f : 0.f) - (s & 2 ? 1.f : 0.f);
    memcpy((uint8_t *)r + 4, &x, 4);
    memcpy((uint8_t *)r + 8, &y, 4);
    ((uint8_t *)r)[12] = 1;
}

static void steam_method(CPU *c) {
    static int trace = -1, skip = -1;   // read once: Steam Input is polled every frame
    if (trace < 0) trace = getenv("CATH_TRACE") != NULL, skip = getenv("CATH_SKIP") != NULL;
    uint32_t slot = (cur_stub - G_STEAM) / 16, w = slot / 64, idx = slot % 64;
    Wrap *wr = &wraps[w];
    const Spec *sp = NULL;
    for (size_t i = 0; i < sizeof specs / sizeof *specs; i++)
        if (specs[i].idx == (int)idx && !strcmp(specs[i].iface, wr->ver)) { sp = &specs[i]; break; }
    if (!sp) die("steam: %s method #%u not bridged yet (called from %08x)", wr->ver, idx, U32(REG(c, R_ESP)));
    if (getenv("CATH_TRACE")) fprintf(stderr, "[steam] %s::%s\n", wr->ver, sp->name);

    // thiscall: this in ECX (ignored, we know the wrapper), args on the stack, callee pops them
    uint32_t slot_i = 0, sret = 0;
    if (strchr("SDA", sp->ret)) sret = ARG(c, slot_i++);
    uint64_t a[8] = {0};
    int n = 0, noop = sp->args[0] == '!';
    const char *ver = NULL;
    for (const char *k = sp->args + noop; *k; k++) {
        if (*k == 'q') { a[n++] = V(GM(REG(c, R_ESP) + 4 + 4 * slot_i), uint64_t); slot_i += 2; continue; }
        uint32_t v = ARG(c, slot_i++);
        if (*k == 'p' || *k == 'I') a[n++] = v ? (uint64_t)(uintptr_t)GM(v) : 0;
        else a[n++] = v;
        if (*k == 'I') ver = GSTR(v);
    }
    // CATH_SKIP tests skip stages nobody cleared: nothing from them reaches the real Steam account
    if (skip && (!strcmp(sp->name, "SetAchievement") || !strcmp(sp->name, "UploadLeaderboardScore"))) {
        fprintf(stderr, "[steam] CATH_SKIP: %s not sent\n", sp->name);
        ret_std(c, sp->ret == 'b', (int)slot_i);   // achievement "set"; leaderboard: no call handle (0 = failed)
        if (sp->ret == 'q') REG(c, R_EDX) = 0;
        return;
    }
    unsigned __int128 r2 = noop ? 0 : VT(wr->native, idx)(wr->native, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if ((!strcmp(sp->name, "GetDigitalActionHandle") || !strcmp(sp->name, "GetAnalogActionHandle")) && a[0]) {
        static char seen[4096];   // log each action name + handle once: shows what the game binds
        const char *nm = (const char *)(uintptr_t)a[0];
        char key[96];
        snprintf(key, sizeof key, "|%s=%llx|", nm, (unsigned long long)(uint64_t)r2);
        if (!strstr(seen, key) && strlen(seen) + strlen(key) < sizeof seen) {
            strcat(seen, key);
            fprintf(stderr, "[steam] input action %s %s -> %llx\n", sp->name + 3, nm, (unsigned long long)(uint64_t)r2);
        }
        action_handle(nm, (uint64_t)r2, sp->name[3] == 'A');
    }
    if (!strcmp(sp->name, "GetConnectedControllers")) {
        static uint64_t seen_pads[2];   // log the controller list when it changes
        uint64_t *h = (uint64_t *)(uintptr_t)a[0];
        uint64_t now_pads[2] = {(uint32_t)r2 > 0 && h ? h[0] : 0, (uint32_t)r2 > 1 && h ? h[1] : 0};
        if ((int)(uint32_t)r2 != steam_input_pads || memcmp(now_pads, seen_pads, sizeof now_pads))
            fprintf(stderr, "[input] steam controllers %u: %llx %llx\n", (uint32_t)r2, (unsigned long long)now_pads[0],
                    (unsigned long long)now_pads[1]);
        memcpy(seen_pads, now_pads, sizeof now_pads);
        steam_input_pads = (int)(uint32_t)r2;
    }
    if (!strcmp(sp->name, "ActivateActionSet")) {
        static struct { uint64_t pad, set; } sets[8];   // log action-set switches per controller (the game repeats them every frame)
        static int n_sets;
        int i = 0;
        while (i < n_sets && sets[i].pad != a[0]) i++;
        if (i == n_sets && n_sets < 8) sets[n_sets].pad = a[0], sets[n_sets++].set = ~0ull;
        if (i < n_sets && sets[i].set != a[1]) {
            sets[i].set = a[1];
            fprintf(stderr, "[input] steam ActivateActionSet pad %llx set %llx (from %08x)\n", (unsigned long long)a[0],
                    (unsigned long long)a[1], U32(REG(c, R_ESP)));
        }
    }
    // LeaderboardEntry_t { CSteamID; int32 rank, score, details; UGCHandle_t }: the first 20 bytes agree; the UGC
    // handle sits at 20 (Linux, pack 4) or 24 (Windows, pack 8). The game attaches no files: report none, Windows layout.
    if (!strcmp(sp->name, "GetDownloadedLeaderboardEntry") && a[2]) {
        memset((uint8_t *)(uintptr_t)a[2] + 20, 0, 4);
        memset((uint8_t *)(uintptr_t)a[2] + 24, 0xFF, 8);   // k_UGCHandleInvalid
    }
    if (!strcmp(sp->name, "GetDigitalActionData")) action_scripted(a[1], &r2), log_digital(a[0], a[1], r2, U32(REG(c, R_ESP)));
    if (!strcmp(sp->name, "GetAnalogActionData")) analog_scripted(a[1], &r2);
    uint64_t r = (uint64_t)r2;
    switch (sp->ret) {
    case 'v': r = 0; break;
    case 'b': r &= 0xFF; break;
    case 'u': r &= 0xFFFFFFFF; break;
    case 'q': REG(c, R_EDX) = (uint32_t)(r >> 32); break;
    case 'S': case 'D': case 'A': memcpy(GM(sret), &r2, sp->ret == 'S' ? 8 : sp->ret == 'D' ? 2 : 13); r = sret; break;
    case 'i': r = wrap((void *)(uintptr_t)r, ver); break;
    case 's': r = r ? guest_cstr((const char *)(uintptr_t)r) : 0; break;
    }
    ret_std(c, (uint32_t)r, (int)slot_i);
}

Fn steam_resolve(uint32_t t) {
    if (t < G_STEAM || t >= G_STEAM + 16 * 64 * MAX_WRAP || (t - G_STEAM) / 16 / 64 >= n_wraps) return NULL;
    cur_stub = t;
    return steam_method;
}

// ---------- flat API ----------

IMP(STEAM, SteamAPI_RestartAppIfNecessary) { RET_CDECL(c, 0); }   // we never relaunch through the Steam UI

IMP(STEAM, SteamAPI_Init) {
    char path[512];
    snprintf(path, sizeof path, "%s/.steam/sdk64/steamclient.so", getenv("HOME") ? getenv("HOME") : "");
    setenv("SteamAppId", "893180", 0);   // tells Steam which game this is when not launched from the Steam UI
    setenv("SteamGameId", "893180", 0);
    if (!(lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))) { fprintf(stderr, "[steam] %s\n", dlerror()); RET_CDECL(c, 0); return; }
    CreateInterface = dlsym(lib, "CreateInterface");
    BGetCallback = dlsym(lib, "Steam_BGetCallback");
    FreeLastCallback = dlsym(lib, "Steam_FreeLastCallback");
    GetAPICallResult = dlsym(lib, "Steam_GetAPICallResult");
    if (!CreateInterface || !(client = CreateInterface("SteamClient017", NULL))) { fprintf(stderr, "[steam] no SteamClient017\n"); RET_CDECL(c, 0); return; }
    pipe_h = (int32_t)VT(client, 0)(client, 0, 0, 0, 0, 0, 0, 0, 0);                  // CreateSteamPipe
    user_h = pipe_h ? (int32_t)VT(client, 2)(client, (uint64_t)pipe_h, 0, 0, 0, 0, 0, 0, 0) : 0;   // ConnectToGlobalUser
    fprintf(stderr, "[steam] pipe %d, user %d\n", pipe_h, user_h);
    RET_CDECL(c, user_h != 0);
}
IMP(STEAM, SteamAPI_GetHSteamPipe) { RET_CDECL(c, (uint32_t)pipe_h); }
IMP(STEAM, SteamAPI_GetHSteamUser) { RET_CDECL(c, (uint32_t)user_h); }
IMP(STEAM, SteamInternal_CreateInterface) {
    const char *ver = GSTR(ARG(c, 0));
    void *p = !strncmp(ver, "SteamClient", 11) ? CreateInterface(ver, NULL)
                                                : (void *)(uintptr_t)VT(client, 12)(client, (uint64_t)user_h, (uint64_t)pipe_h, (uint64_t)(uintptr_t)ver, 0, 0, 0, 0, 0);
    RET_CDECL(c, wrap(p, ver));
}
// SteamInternal_ContextInit(data): data = {void (*init)(ctx), uintptr counter, CSteamAPIContext ctx}; init runs once.
IMP(STEAM, SteamInternal_ContextInit) {
    uint32_t d = ARG(c, 0);
    if (U32(d + 4) != 1) {
        uint32_t esp = REG(c, R_ESP);
        guest_call(c, U32(d), 0, 1, d + 8);
        REG(c, R_ESP) = esp;
        U32(d + 4) = 1;
    }
    RET_CDECL(c, d + 8);
}

// ---------- async call results ----------

typedef struct { uint32_t cb; uint64_t call; } Pending;
static Pending pending[256];
static uint32_t n_pending;

IMP(STEAM, SteamAPI_RegisterCallResult) {   // (CCallbackBase*, SteamAPICall_t)
    if (n_pending < 256) pending[n_pending++] = (Pending){ARG(c, 0), V(GM(REG(c, R_ESP) + 8), uint64_t)};
    RET_CDECL(c, 0);
}
IMP(STEAM, SteamAPI_UnregisterCallResult) {
    uint32_t cb = ARG(c, 0);
    uint64_t call = V(GM(REG(c, R_ESP) + 8), uint64_t);
    for (uint32_t i = 0; i < n_pending; i++)
        if (pending[i].cb == cb && pending[i].call == call) { pending[i] = pending[--n_pending]; break; }
    RET_CDECL(c, 0);
}
// CATH_SKIP=1 (automated tests only): the puzzle pause menu offers Skip only on a stage with a Gold prize
// (0x702080: save[0x3257 + difficulty * 13 + stage] == 3). Mark the current stage so a test can skip a real-time
// stage it cannot play. Changes the port's own save copy only (once the game saves); see the achievement block below.
static void skip_test(void) {
    static int on = -1;
    if (on < 0) on = getenv("CATH_SKIP") != NULL;
    uint32_t save = on ? U32(0xd19a40) : 0, pz = on ? U32(0xd52ce4) : 0;
    static char wflag[256];   // test runs: creating ~/catherine-port/waiters.on prints every blocked thread (sync.c)
    if (on && !wflag[0]) snprintf(wflag, sizeof wflag, "%s/catherine-port/waiters.on", getenv("HOME"));
    if (on && !unlink(wflag)) dump_waiters();
    if (!save) return;
    uint32_t diff = U32(save + 0x44c8), stage = pz ? U32(pz + 0x18) : 99;
    if (diff < 4 && stage < 13) *(uint8_t *)GM(save + 0x3257 + diff * 13 + stage) = 3;
    // CATH_BABEL=1: Babel stages 1-4 unlocked (flags 0x600-0x603 = save+8 bits 0-3; the game sets them after a stage
    // clear once 1/3/5/9 stages have Gold on Normal or Hard, 0x75fa78; the stage select reads them at 0x5604d3)
    static int babel = -1;
    if (babel < 0) babel = getenv("CATH_BABEL") != NULL;
    if (babel) *(uint32_t *)GM(save + 8) |= 0xf;
    // CATH_KARMA=0..255: hold the karma meter (save+0x42c8, starts at 128, answers add/subtract via 0x6980f0; the
    // final confessional 0x69bb83 branches on > 163 / <= 95) to reach the other endings
    static int karma = -2;
    if (karma == -2) karma = getenv("CATH_KARMA") ? atoi(getenv("CATH_KARMA")) : -1;
    if (karma >= 0) *(uint32_t *)GM(save + 0x42c8) = karma;
    // test runs: creating ~/catherine-port/dump.on writes the save block to save_<n>.bin (diffs find story values)
    static char flag[256];
    static int n;
    if (!flag[0]) snprintf(flag, sizeof flag, "%s/catherine-port/dump.on", getenv("HOME"));
    if (unlink(flag)) return;
    char path[256];
    snprintf(path, sizeof path, "%s/catherine-port/save_%d.bin", getenv("HOME"), n++);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(GM(save), 1, 0x8000, f), fclose(f);
}
IMP(STEAM, SteamAPI_RunCallbacks) {
    skip_test();
    struct { int32_t user, id; uint8_t *param; int32_t size; } msg;   // CallbackMsg_t (64-bit layout)
    while (BGetCallback && BGetCallback(pipe_h, &msg)) {
        if (msg.id == 703) {   // SteamAPICallCompleted_t {uint64 call; int callback; uint32 size}
            uint64_t call = *(uint64_t *)msg.param;
            int32_t id = *(int32_t *)(msg.param + 8), size = *(int32_t *)(msg.param + 12);
            for (uint32_t i = 0; i < n_pending; i++) {
                if (pending[i].call != call) continue;
                uint32_t cb = pending[i].cb, buf = gmalloc(size + 16), esp = REG(c, R_ESP);
                uint8_t failed = 0;
                pending[i] = pending[--n_pending];
                // Steam packs callback structs to 8 bytes on Windows but 4 on Linux: the bytes copy as-is only where no
                // 8-byte field follows a smaller one (true for the results bridged so far); others are re-laid-out here
                if (!GetAPICallResult(pipe_h, call, GM(buf), size, id, &failed)) failed = 1;
                if (id == 1106 && !failed) {   // LeaderboardScoreUploaded_t: u8 ok; u64 lb; i32 score; u8 changed; i32 new, prev
                    uint8_t l[28], *w = GM(buf);
                    memcpy(l, w, 28);
                    memset(w, 0, 32);
                    w[0] = l[0];
                    memcpy(w + 8, l + 4, 8);
                    memcpy(w + 16, l + 12, 4);
                    w[20] = l[16];
                    memcpy(w + 24, l + 20, 8);
                }
                // MSVC puts overloaded virtuals in reverse order: vtable[0] = Run(void*, bool, SteamAPICall_t)
                REG(c, R_ECX) = cb;
                guest_call(c, U32(U32(cb)), 1, 4, buf, (uint32_t)failed, (uint32_t)call, (uint32_t)(call >> 32));
                REG(c, R_ESP) = esp;
                gfree(buf);
                break;
            }
        }
        FreeLastCallback(pipe_h);
    }
    RET_CDECL(c, 0);
}
