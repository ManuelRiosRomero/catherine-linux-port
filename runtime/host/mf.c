// Media Foundation, replaced at the player seam. The game's movie class (AwWmvWin32Movie) owns a WmvPlayer (MS
// "Simple Player" sample, 0x22c bytes). Only WmvPlayer::OpenURL (0x6a96d0) is native: instead of building an MF
// pipeline it opens the file with runtime/movie.c (ffmpeg) and hands the player four small fake COM objects. The rest
// of the player and movie code stays translated and drives those objects: session Start/Pause/Close/Shutdown,
// IMFVideoDisplayControl::GetCurrentImage (a bottom-up 32-bit DIB of the current frame), IMFAudioStreamVolume.
// Audio plays through an SDL3 audio stream; the movie clock is the host clock while playing.
#include "host.h"
#include "../movie.h"
#include <SDL3/SDL.h>
#include <string.h>

#define MF "MF.dll"
// WmvPlayer fields the translated code reads
#define P_URL 0x4           // wchar[255]
#define P_SESSION 0x208     // IMFMediaSession*
#define P_SOURCE 0x20c      // IMFMediaSource*
#define P_DISPLAY 0x210     // IMFVideoDisplayControl*
#define P_VOLUME 0x214      // IMFAudioStreamVolume*
#define P_STATE 0x220       // 0 closed, 1 ready, 2 open pending, 3 started, 4 paused, 5 stopped (= reached the end), 6 closing
#define P_CLOSE_EVENT 0x224 // signaled when the session has closed
#define E_FAIL 0x80004005u

typedef struct {
    uint32_t player;            // guest WmvPlayer, 0 = free slot
    Movie *m;
    SDL_AudioStream *audio;
    int w, h, have_frame, running;
    uint8_t *frame;             // current frame, 32-bit BGRA, top row first
    uint64_t base_us, start_ns; // movie clock: base_us + time since start_ns while running
    float vol[2];
} Player;
static Player players[4];       // ponytail: the game plays one movie at a time; 4 slots is headroom

static Player *find(uint32_t player) {
    for (int i = 0; i < 4; i++) if (players[i].player == player) return &players[i];
    return NULL;
}
static Player *of(uint32_t obj) { return find(U32(obj + 4)); }   // fake object: +0 vtable, +4 owning WmvPlayer
static uint64_t clock_us(Player *p) { return p->base_us + (p->running ? (SDL_GetTicksNS() - p->start_ns) / 1000 : 0); }

static void SDLCALL feed(void *u, SDL_AudioStream *s, int need, int total) {   // SDL audio thread
    Player *p = u;
    float buf[2 * 2048];
    (void)total;
    while (need > 0) {
        int n = movie_audio(p->m, buf, need / 8 < 2048 ? need / 8 + 1 : 2048);
        if (n <= 0) break;
        SDL_PutAudioStreamData(s, buf, n * 8);
        need -= n * 8;
    }
}

static void run(Player *p, int on) {
    if (on == p->running) return;
    if (on) p->start_ns = SDL_GetTicksNS();
    else p->base_us = clock_us(p);
    p->running = on;
    if (p->audio) on ? SDL_ResumeAudioStreamDevice(p->audio) : SDL_PauseAudioStreamDevice(p->audio);
}

static void player_free(Player *p) {
    if (p->audio) SDL_DestroyAudioStream(p->audio);   // stops the callback before the movie goes away
    movie_close(p->m);
    free(p->frame);
    memset(p, 0, sizeof *p);
}

// The vtable (one named stub per method) is built once per interface: building it per movie used up the 4096-entry
// import table after ~70 movies (named stubs are never shared or freed).
static uint32_t make_obj(const char *iface, const char **names, int n, uint32_t player) {
    static struct { const char **names; uint32_t vt; } cache[8];
    uint32_t vt = 0, o = gmalloc(8);
    for (int i = 0; i < 8 && cache[i].names; i++) if (cache[i].names == names) vt = cache[i].vt;
    if (!vt) {
        char buf[96];
        vt = gmalloc(4 * n);
        for (int i = 0; i < n; i++) {
            snprintf(buf, sizeof buf, "%s::%s", iface, names[i]);
            U32(vt + 4 * i) = named_stub(MF, strdup(buf));
        }
        for (int i = 0; i < 8; i++) if (!cache[i].names) { cache[i].names = names; cache[i].vt = vt; break; }
    }
    U32(o) = vt;
    U32(o + 4) = player;
    return o;
}
static const char *session_m[] = {"QueryInterface", "AddRef", "Release", "GetEvent", "BeginGetEvent", "EndGetEvent",
    "QueueEvent", "SetTopology", "ClearTopologies", "Start", "Pause", "Stop", "Close", "Shutdown", "GetClock",
    "GetSessionCapabilities", "GetFullTopology"};
static const char *source_m[] = {"QueryInterface", "AddRef", "Release", "GetEvent", "BeginGetEvent", "EndGetEvent",
    "QueueEvent", "GetCharacteristics", "CreatePresentationDescriptor", "Start", "Stop", "Pause", "Shutdown"};
static const char *display_m[] = {"QueryInterface", "AddRef", "Release", "GetNativeVideoSize", "GetIdealVideoSize",
    "SetVideoPosition", "GetVideoPosition", "SetAspectRatioMode", "GetAspectRatioMode", "SetVideoWindow",
    "GetVideoWindow", "RepaintVideo", "GetCurrentImage"};
static const char *volume_m[] = {"QueryInterface", "AddRef", "Release", "GetChannelCount", "SetChannelVolume",
    "GetChannelVolume", "SetAllVolumes", "GetAllVolumes"};

// The player's own pipeline code (topology, renderers, its IMFAsyncCallback) only runs on Media Foundation session
// events. The fake session never sends any (its BeginGetEvent is a stop-stub), so these are never reached.
#define MF_DEAD(dll, name) IMP(dll, name) { die(dll "!" #name ": Media Foundation pipeline code, replaced by mf.c"); }
MF_DEAD(MF, MFGetService) MF_DEAD(MF, MFCreateTopology) MF_DEAD(MF, MFCreateTopologyNode)
MF_DEAD(MF, MFCreateAudioRendererActivate) MF_DEAD(MF, MFCreateVideoRendererActivate)
IMPX("SHLWAPI.dll", "#219", QISearch) { die("SHLWAPI.dll!QISearch: WmvPlayer::QueryInterface, only Media Foundation calls it"); }
IMP("MFPlat.DLL", MFStartup) { ret_std(c, 0, 2); }
IMP("MFPlat.DLL", MFShutdown) { ret_std(c, 0, 0); }

// WmvPlayer::OpenURL(this = ECX, const wchar_t *url, bool flag), callee pops 8 bytes
NATIVE(0x6a96d0, WmvPlayer_OpenURL) {
    uint32_t pl = REG(c, R_ECX), url = ARG(c, 0);
    char win[512], path[1024];
    int n = 0;
    for (; n < 254 && V(GM(url + 2 * n), uint16_t); n++) {
        uint16_t ch = V(GM(url + 2 * n), uint16_t);
        V(GM(pl + P_URL + 2 * n), uint16_t) = ch;
        win[n] = ch < 0x80 ? (char)ch : '?';
    }
    V(GM(pl + P_URL + 2 * n), uint16_t) = 0;
    win[n] = 0;
    V(GM(0xd3c259), uint8_t) = (uint8_t)ARG(c, 1);   // the original stores its flag argument in this global
    Player *p = find(pl);
    if (p) player_free(p);                            // reopen: the old fake objects are simply dropped
    for (int i = 0; i < 4 && !p; i++) if (!players[i].player) p = &players[i];
    if (!p) die("WmvPlayer::OpenURL: more than 4 players");
    host_path(win, path, sizeof path);
    Movie *m = movie_open(path);
    if (!m) {
        fprintf(stderr, "[mf] cannot open movie %s\n", path);
        U32(pl + P_STATE) = 0;
        ret_std(c, E_FAIL, 2);
        return;
    }
    p->player = pl;
    p->m = m;
    movie_size(m, &p->w, &p->h);
    p->frame = calloc((size_t)p->w * p->h, 4);
    p->vol[0] = p->vol[1] = 1;
    static int audio_ok = -1;
    if (audio_ok < 0 && !(audio_ok = SDL_InitSubSystem(SDL_INIT_AUDIO))) fprintf(stderr, "[mf] no audio: %s\n", SDL_GetError());
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};   // movie_audio's format
    if (audio_ok && !(p->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, p)))
        fprintf(stderr, "[mf] audio stream: %s\n", SDL_GetError());
    U32(pl + P_SESSION) = make_obj("IMFMediaSession", session_m, 17, pl);
    U32(pl + P_SOURCE) = make_obj("IMFMediaSource", source_m, 13, pl);
    U32(pl + P_DISPLAY) = make_obj("IMFVideoDisplayControl", display_m, 13, pl);
    U32(pl + P_VOLUME) = make_obj("IMFAudioStreamVolume", volume_m, 8, pl);
    // MF would now open asynchronously and start; the movie class starts paused, so go straight to "paused at 0"
    U32(pl + P_STATE) = 4;
    fprintf(stderr, "[mf] open %s (%dx%d) at %.1f s\n", path, p->w, p->h, SDL_GetTicks() / 1000.0);
    ret_std(c, 0, 2);
}

// COM methods: stdcall, `this` is the first stack argument
#define M(iface, name, nargs) IMPX(MF, #iface "::" #name, iface##_##name)
M(IMFMediaSession, Release, 1) { gfree(ARG(c, 0)); ret_std(c, 0, 1); }   // each object has exactly one owner
M(IMFMediaSource, Release, 1) { gfree(ARG(c, 0)); ret_std(c, 0, 1); }
M(IMFVideoDisplayControl, Release, 1) { gfree(ARG(c, 0)); ret_std(c, 0, 1); }
M(IMFAudioStreamVolume, Release, 1) { gfree(ARG(c, 0)); ret_std(c, 0, 1); }

M(IMFMediaSession, Start, 3) {   // (this, time format GUID, start position): resume, or restart after the end
    Player *p = of(ARG(c, 0));
    if (p) {
        uint32_t pl = p->player;
        if (U32(pl + P_STATE) == 5) {
            if (p->audio) SDL_LockAudioStream(p->audio);
            movie_rewind(p->m);
            if (p->audio) { SDL_ClearAudioStream(p->audio); SDL_UnlockAudioStream(p->audio); }
            p->base_us = 0;
        }
        run(p, 1);
        U32(pl + P_STATE) = 3;
    }
    ret_std(c, p ? 0 : E_FAIL, 3);
}
M(IMFMediaSession, Pause, 1) { Player *p = of(ARG(c, 0)); if (p) run(p, 0); ret_std(c, 0, 1); }   // caller sets state 4
M(IMFMediaSession, Close, 1) {
    Player *p = of(ARG(c, 0));
    if (p) { run(p, 0); event_signal(U32(p->player + P_CLOSE_EVENT)); }   // Shutdown (0x6a9c80) waits for this
    ret_std(c, 0, 1);
}
M(IMFMediaSession, Shutdown, 1) { Player *p = of(ARG(c, 0)); if (p) player_free(p); ret_std(c, 0, 1); }
M(IMFMediaSource, Shutdown, 1) { ret_std(c, 0, 1); }

M(IMFVideoDisplayControl, GetNativeVideoSize, 3) {   // (this, SIZE *video, SIZE *aspect ratio): 0x6a8b70 fits the video
    Player *p = of(ARG(c, 0));
    for (int i = 1; i <= 2; i++)
        if (p && ARG(c, i)) { U32(ARG(c, i)) = i == 1 ? p->w : 1; U32(ARG(c, i) + 4) = i == 1 ? p->h : 1; }   // square pixels
    ret_std(c, p ? 0 : E_FAIL, 3);
}
M(IMFVideoDisplayControl, SetVideoPosition, 3) { ret_std(c, 0, 3); }   // the frame goes to a texture, not a window
// (this, BITMAPINFOHEADER *bih, BYTE **dib, DWORD *size, LONGLONG *time): the caller frees *dib with CoTaskMemFree
M(IMFVideoDisplayControl, GetCurrentImage, 5) {
    Player *p = of(ARG(c, 0));
    if (!p) { ret_std(c, E_FAIL, 5); return; }
    uint64_t pos = clock_us(p);
    int r = movie_frame_at(p->m, (int64_t)pos, p->frame, p->w * 4, p->w, p->h);
    if (r > 0) p->have_frame = 1;
    static int fast = -1;   // CATH_FASTMOVIE=1 (test runs): every movie ends 0.5 s in, as if it were that short
    if (fast < 0) fast = getenv("CATH_FASTMOVIE") != NULL;
    if (fast && p->have_frame && pos > 500000) r = -1;
    if (r < 0) { run(p, 0); U32(p->player + P_STATE) = 5; }   // end of presentation: the movie class loops or finishes
    if (!p->have_frame) { ret_std(c, E_FAIL, 5); return; }
    if (getenv("CATH_TRACE_MF")) {   // the caller (0x6a9860) copies into this texture lock: {pixel data, ptr, pitch}
        uint32_t ebp = REG(c, 0x14), lock = U32(ebp + 8);
        fprintf(stderr, "[mf] frame %llu us: lock ptr %08x (heap block %u bytes) pitch %u, texture %ux%u\n", (unsigned long long)pos,
                U32(lock + 4), gsize(U32(lock + 4)), U32(lock + 8), REG(c, 0xc), U32(ebp - 0x4c));
    }
    uint32_t row = p->w * 4, size = row * p->h, dib = gmalloc(size), bih = ARG(c, 1);
    // the movie object (AwWmvWin32Movie, vtable 0xb964c0) owning this player: its frame update 0x6a8c30 calls our caller
    // 0x6a9860 (EBP frame, returns to 0x6a8d0a) with ESI = movie, saved at [EBP-0x5c]; +0x24 = draws its own full-canvas
    // sprite, +0x58 = its WmvPlayer. Not found = treated as an own sprite (letterboxed as before).
    uint32_t mebp = REG(c, R_EBP), mv = U32(mebp + 4) == 0x6a8d0a ? U32(mebp - 0x5c) : 0;
    if (mv && (U32(mv) != 0xb964c0 || U32(mv + 0x58) != p->player)) mv = 0;
    int ch = aspect_movie_rows(p->w, p->h, mv ? U32(mv + 0x24) != 0 : 1), top = (p->h - ch) / 2;   // 16:10: picture rows
    if (ch == p->h)
        for (int y = 0; y < p->h; y++) memcpy(GM(dib + y * row), p->frame + (size_t)(p->h - 1 - y) * row, row);   // bottom-up
    else {
        memset(GM(dib), 0, size);
        for (int y = 0; y < ch; y++) {   // linear between the two nearest source rows
            float sy = (y + 0.5f) * p->h / ch - 0.5f;
            int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < p->h ? y0 + 1 : y0;
            unsigned f = (unsigned)((sy - y0) * 256), g = 256 - f;
            const uint8_t *a = p->frame + (size_t)y0 * row, *b2 = p->frame + (size_t)y1 * row;
            uint8_t *d = GM(dib + (p->h - 1 - (top + y)) * row);
            for (uint32_t k = 0; k < row; k++) d[k] = (uint8_t)((a[k] * g + b2[k] * f) >> 8);
        }
    }
    int32_t *b = (int32_t *)GM(bih);
    b[0] = 40; b[1] = p->w; b[2] = p->h; b[3] = 1 | 32 << 16; b[4] = 0; b[5] = (int32_t)size;
    b[6] = b[7] = b[8] = b[9] = 0;
    U32(ARG(c, 2)) = dib;
    U32(ARG(c, 3)) = size;
    V(GM(ARG(c, 4)), uint64_t) = pos * 10;   // 100 ns units
    ret_std(c, 0, 5);
}

M(IMFAudioStreamVolume, GetChannelCount, 2) { U32(ARG(c, 1)) = 2; ret_std(c, 0, 2); }
M(IMFAudioStreamVolume, SetChannelVolume, 3) {   // (this, channel, float level)
    Player *p = of(ARG(c, 0));
    uint32_t ch = ARG(c, 1), bits = ARG(c, 2);
    if (p && ch < 2) {
        memcpy(&p->vol[ch], &bits, 4);
        if (p->audio) SDL_SetAudioStreamGain(p->audio, p->vol[0] > p->vol[1] ? p->vol[0] : p->vol[1]);
    }
    ret_std(c, 0, 3);
}
