// USER32 on SDL3: window classes, windows, the message queue, keyboard/mouse state, cursor, metrics.
// The game's top-level window becomes an SDL window (Vulkan-capable, for D3D9 later); child/popup windows are
// bookkeeping only. PeekMessage pumps SDL events and turns them into WM_* messages for the game's window procedure.
// ponytail: one message queue for the whole process (the game pumps messages on one thread).
#define _GNU_SOURCE
#include "host.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
// CATH_TRACE_WIN=1: count the window/cursor calls that reach the display server (one line per 500 calls each)
#define WCOUNT(name) do { static int on = -1, n; if (on < 0) on = getenv("CATH_TRACE_WIN") != NULL; \
    if (on && !(++n % 500)) fprintf(stderr, "[user32] %s x%d at %.1f s\n", name, n, SDL_GetTicks() / 1000.0); } while (0)

#define U "USER32.dll"

// ---------- keys: SDL scancode <-> Windows virtual key <-> PS/2 set-1 scan code ----------

typedef struct { SDL_Scancode sdl; uint8_t vk, scan, ext; const char *name; } Key;
static const Key keys[] = {
    {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, 0, "Esc"}, {SDL_SCANCODE_1, '1', 0x02, 0, "1"}, {SDL_SCANCODE_2, '2', 0x03, 0, "2"},
    {SDL_SCANCODE_3, '3', 0x04, 0, "3"}, {SDL_SCANCODE_4, '4', 0x05, 0, "4"}, {SDL_SCANCODE_5, '5', 0x06, 0, "5"},
    {SDL_SCANCODE_6, '6', 0x07, 0, "6"}, {SDL_SCANCODE_7, '7', 0x08, 0, "7"}, {SDL_SCANCODE_8, '8', 0x09, 0, "8"},
    {SDL_SCANCODE_9, '9', 0x0A, 0, "9"}, {SDL_SCANCODE_0, '0', 0x0B, 0, "0"}, {SDL_SCANCODE_MINUS, 0xBD, 0x0C, 0, "-"},
    {SDL_SCANCODE_EQUALS, 0xBB, 0x0D, 0, "="}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, 0, "Backspace"},
    {SDL_SCANCODE_TAB, 0x09, 0x0F, 0, "Tab"}, {SDL_SCANCODE_Q, 'Q', 0x10, 0, "Q"}, {SDL_SCANCODE_W, 'W', 0x11, 0, "W"},
    {SDL_SCANCODE_E, 'E', 0x12, 0, "E"}, {SDL_SCANCODE_R, 'R', 0x13, 0, "R"}, {SDL_SCANCODE_T, 'T', 0x14, 0, "T"},
    {SDL_SCANCODE_Y, 'Y', 0x15, 0, "Y"}, {SDL_SCANCODE_U, 'U', 0x16, 0, "U"}, {SDL_SCANCODE_I, 'I', 0x17, 0, "I"},
    {SDL_SCANCODE_O, 'O', 0x18, 0, "O"}, {SDL_SCANCODE_P, 'P', 0x19, 0, "P"}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, 0, "["},
    {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, 0, "]"}, {SDL_SCANCODE_RETURN, 0x0D, 0x1C, 0, "Enter"},
    {SDL_SCANCODE_LCTRL, 0x11, 0x1D, 0, "Ctrl"}, {SDL_SCANCODE_A, 'A', 0x1E, 0, "A"}, {SDL_SCANCODE_S, 'S', 0x1F, 0, "S"},
    {SDL_SCANCODE_D, 'D', 0x20, 0, "D"}, {SDL_SCANCODE_F, 'F', 0x21, 0, "F"}, {SDL_SCANCODE_G, 'G', 0x22, 0, "G"},
    {SDL_SCANCODE_H, 'H', 0x23, 0, "H"}, {SDL_SCANCODE_J, 'J', 0x24, 0, "J"}, {SDL_SCANCODE_K, 'K', 0x25, 0, "K"},
    {SDL_SCANCODE_L, 'L', 0x26, 0, "L"}, {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, 0, ";"}, {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, 0, "'"},
    {SDL_SCANCODE_GRAVE, 0xC0, 0x29, 0, "`"}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2A, 0, "Shift"},
    {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, 0, "\\"}, {SDL_SCANCODE_Z, 'Z', 0x2C, 0, "Z"}, {SDL_SCANCODE_X, 'X', 0x2D, 0, "X"},
    {SDL_SCANCODE_C, 'C', 0x2E, 0, "C"}, {SDL_SCANCODE_V, 'V', 0x2F, 0, "V"}, {SDL_SCANCODE_B, 'B', 0x30, 0, "B"},
    {SDL_SCANCODE_N, 'N', 0x31, 0, "N"}, {SDL_SCANCODE_M, 'M', 0x32, 0, "M"}, {SDL_SCANCODE_COMMA, 0xBC, 0x33, 0, ","},
    {SDL_SCANCODE_PERIOD, 0xBE, 0x34, 0, "."}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, 0, "/"},
    {SDL_SCANCODE_RSHIFT, 0x10, 0x36, 0, "Right Shift"}, {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37, 0, "Num *"},
    {SDL_SCANCODE_LALT, 0x12, 0x38, 0, "Alt"}, {SDL_SCANCODE_SPACE, 0x20, 0x39, 0, "Space"},
    {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A, 0, "Caps Lock"}, {SDL_SCANCODE_F1, 0x70, 0x3B, 0, "F1"}, {SDL_SCANCODE_F2, 0x71, 0x3C, 0, "F2"},
    {SDL_SCANCODE_F3, 0x72, 0x3D, 0, "F3"}, {SDL_SCANCODE_F4, 0x73, 0x3E, 0, "F4"}, {SDL_SCANCODE_F5, 0x74, 0x3F, 0, "F5"},
    {SDL_SCANCODE_F6, 0x75, 0x40, 0, "F6"}, {SDL_SCANCODE_F7, 0x76, 0x41, 0, "F7"}, {SDL_SCANCODE_F8, 0x77, 0x42, 0, "F8"},
    {SDL_SCANCODE_F9, 0x78, 0x43, 0, "F9"}, {SDL_SCANCODE_F10, 0x79, 0x44, 0, "F10"}, {SDL_SCANCODE_F11, 0x7A, 0x57, 0, "F11"},
    {SDL_SCANCODE_F12, 0x7B, 0x58, 0, "F12"}, {SDL_SCANCODE_KP_7, 0x67, 0x47, 0, "Num 7"}, {SDL_SCANCODE_KP_8, 0x68, 0x48, 0, "Num 8"},
    {SDL_SCANCODE_KP_9, 0x69, 0x49, 0, "Num 9"}, {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A, 0, "Num -"}, {SDL_SCANCODE_KP_4, 0x64, 0x4B, 0, "Num 4"},
    {SDL_SCANCODE_KP_5, 0x65, 0x4C, 0, "Num 5"}, {SDL_SCANCODE_KP_6, 0x66, 0x4D, 0, "Num 6"}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E, 0, "Num +"},
    {SDL_SCANCODE_KP_1, 0x61, 0x4F, 0, "Num 1"}, {SDL_SCANCODE_KP_2, 0x62, 0x50, 0, "Num 2"}, {SDL_SCANCODE_KP_3, 0x63, 0x51, 0, "Num 3"},
    {SDL_SCANCODE_KP_0, 0x60, 0x52, 0, "Num 0"}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53, 0, "Num Del"},
    {SDL_SCANCODE_KP_ENTER, 0x0D, 0x1C, 1, "Num Enter"}, {SDL_SCANCODE_RCTRL, 0x11, 0x1D, 1, "Right Ctrl"},
    {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0x35, 1, "Num /"}, {SDL_SCANCODE_RALT, 0x12, 0x38, 1, "Right Alt"},
    {SDL_SCANCODE_HOME, 0x24, 0x47, 1, "Home"}, {SDL_SCANCODE_UP, 0x26, 0x48, 1, "Up"}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, 1, "Page Up"},
    {SDL_SCANCODE_LEFT, 0x25, 0x4B, 1, "Left"}, {SDL_SCANCODE_RIGHT, 0x27, 0x4D, 1, "Right"}, {SDL_SCANCODE_END, 0x23, 0x4F, 1, "End"},
    {SDL_SCANCODE_DOWN, 0x28, 0x50, 1, "Down"}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, 1, "Page Down"},
    {SDL_SCANCODE_INSERT, 0x2D, 0x52, 1, "Insert"}, {SDL_SCANCODE_DELETE, 0x2E, 0x53, 1, "Delete"},
};
#define NKEYS (sizeof keys / sizeof *keys)
static const Key *key_by_sdl(SDL_Scancode s) { for (size_t i = 0; i < NKEYS; i++) if (keys[i].sdl == s) return &keys[i]; return NULL; }
static const Key *key_by_vk(uint32_t vk) { for (size_t i = 0; i < NKEYS; i++) if (keys[i].vk == vk) return &keys[i]; return NULL; }
static const Key *key_by_scan(uint32_t sc, int ext) {
    for (size_t i = 0; i < NKEYS; i++) if (keys[i].scan == sc && keys[i].ext == ext) return &keys[i];
    return NULL;
}

// ---------- classes and windows ----------

typedef struct { char name[64]; uint32_t wndproc, style, extra; } Class;
typedef struct {
    int used, visible, cls;
    uint32_t wndproc, style, exstyle, parent, userdata, id, inst;
    int x, y, w, h;
    uint8_t extra[64];
    SDL_Window *sdl;
} Win;
#define MAX_WIN 64
#define HWND0 0x00010010u                 // hwnd i = HWND0 + 16 * i
#define HWND_DESKTOP 0x00010000u
static Class classes[64];
static int n_classes;
static Win wins[MAX_WIN];
static uint32_t focus_hwnd, main_hwnd;

static Win *win(uint32_t h) {
    uint32_t i = (h - HWND0) / 16;
    return h >= HWND0 && (h - HWND0) % 16 == 0 && i < MAX_WIN && wins[i].used ? &wins[i] : NULL;
}
SDL_Window *hwnd_sdl(uint32_t h) { Win *w = win(h); return w ? w->sdl : NULL; }
// for the D3D9 layer: the SDL window a guest HWND renders into (the game renders into a child of its main window)
SDL_Window *hwnd_window(uint32_t h) {
    for (int depth = 0; depth < 8; depth++) {
        Win *w = win(h);
        if (!w) return NULL;
        if (w->sdl) return w->sdl;
        h = w->parent;
    }
    return NULL;
}

static void sdl_up(void) {
    static int done;
    if (done) return;
    done = 1;
    // Native Wayland under KWin dies now and then: "wp_linux_drm_syncobj_surface_v1: explicit sync is used, but no
    // release point is set" -> the compositor drops the connection -> SDL_EVENT_QUIT. XWayland has no such protocol
    // error, and Game Mode (gamescope) runs games on X11 anyway. A hint, not setenv: SDL copied the environment when
    // the gamepad code started it before main. SDL_VIDEO_DRIVER in the environment still wins over a default hint.
    if (getenv("DISPLAY")) SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11", SDL_HINT_DEFAULT);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {   // no X server we may use (e.g. no XAUTHORITY): SDL's own pick
        fprintf(stderr, "[user32] x11: %s, trying SDL's default video driver\n", SDL_GetError());
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) die("SDL_Init: %s", SDL_GetError());
    }
    fprintf(stderr, "[user32] video: %s\n", SDL_GetCurrentVideoDriver());
}
static void screen_size(int *w, int *h) {
    sdl_up();
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    *w = m ? m->w : 1280;
    *h = m ? m->h : 800;
}

static void register_class(CPU *c, uint32_t wc, int ex, int nargs) {   // WNDCLASS(EX)A
    uint32_t base = ex ? wc + 4 : wc;                                      // EX has cbSize first
    if (n_classes == 64) die("too many window classes");
    Class *k = &classes[n_classes];
    k->style = U32(base);
    k->wndproc = U32(base + 4);
    k->extra = U32(base + 12);                                              // cbWndExtra
    uint32_t name = U32(base + 36);                                         // lpszClassName
    if (name < 0x10000) snprintf(k->name, sizeof k->name, "#%u", name);
    else snprintf(k->name, sizeof k->name, "%s", GSTR(name));
    ret_std(c, 0xC000 + n_classes++, nargs);                               // class atom
}
IMP(U, RegisterClassA) { register_class(c, ARG(c, 0), 0, 1); }
IMP(U, RegisterClassExA) { register_class(c, ARG(c, 0), 1, 1); }

static uint32_t send(CPU *c, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win *w = win(h);
    if (!w || !w->wndproc) return 0;
    uint32_t esp = REG(c, R_ESP), r = guest_call(c, w->wndproc, 1, 4, h, msg, wp, lp);
    REG(c, R_ESP) = esp;
    return r;
}

IMP(U, CreateWindowExA) {   // (exstyle, class, title, style, x, y, w, h, parent, menu, inst, param)
    uint32_t cls_arg = ARG(c, 1), style = ARG(c, 3), parent = ARG(c, 8);
    char cname[64];
    if (cls_arg < 0x10000) snprintf(cname, sizeof cname, "#%u", cls_arg); else snprintf(cname, sizeof cname, "%s", GSTR(cls_arg));
    int k = -1;
    for (int i = 0; i < n_classes; i++)
        if (!strcasecmp(classes[i].name, cname) || (cls_arg >= 0xC000 && cls_arg < 0xC000u + n_classes && i == (int)(cls_arg - 0xC000))) k = i;
    if (k < 0) { fprintf(stderr, "[user32] CreateWindowExA: unknown class %s\n", cname); ret_std(c, 0, 12); return; }
    int i = 0;
    while (i < MAX_WIN && wins[i].used) i++;
    if (i == MAX_WIN) die("too many windows");
    Win *w = &wins[i];
    memset(w, 0, sizeof *w);
    uint32_t h = HWND0 + 16 * i;
    int sw, sh;
    screen_size(&sw, &sh);
    w->used = 1;
    w->cls = k;
    w->wndproc = classes[k].wndproc;
    w->style = style;
    w->exstyle = ARG(c, 0);
    w->parent = parent;
    w->inst = ARG(c, 10);
    w->x = (int32_t)ARG(c, 4) == (int32_t)0x80000000 ? 0 : (int32_t)ARG(c, 4);   // CW_USEDEFAULT
    w->y = (int32_t)ARG(c, 5) == (int32_t)0x80000000 ? 0 : (int32_t)ARG(c, 5);
    w->w = (int32_t)ARG(c, 6) == (int32_t)0x80000000 ? sw : (int32_t)ARG(c, 6);
    w->h = (int32_t)ARG(c, 7) == (int32_t)0x80000000 ? sh : (int32_t)ARG(c, 7);
    const char *title = ARG(c, 2) ? GSTR(ARG(c, 2)) : "";
    if (!parent && !(style & 0x40000000)) {   // top-level, not WS_CHILD: a real window
        w->sdl = SDL_CreateWindow(title, w->w > 0 ? w->w : sw, w->h > 0 ? w->h : sh, SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!w->sdl) die("SDL_CreateWindow: %s", SDL_GetError());
        if (!main_hwnd) main_hwnd = h, aspect_init(sw, sh);
    }
    fprintf(stderr, "[user32] CreateWindowExA class=%s title=\"%s\" %dx%d style=%08x parent=%08x -> %08x%s\n",
            cname, title, w->w, w->h, style, parent, h, w->sdl ? " (SDL window)" : "");
    // CREATESTRUCTA for WM_NCCREATE / WM_CREATE
    uint32_t cs = gmalloc(48);
    U32(cs) = ARG(c, 11); U32(cs + 4) = w->inst; U32(cs + 8) = ARG(c, 9); U32(cs + 12) = parent;
    U32(cs + 16) = w->h; U32(cs + 20) = w->w; U32(cs + 24) = w->y; U32(cs + 28) = w->x;
    U32(cs + 32) = style; U32(cs + 36) = ARG(c, 2); U32(cs + 40) = cls_arg; U32(cs + 44) = w->exstyle;
    if (!send(c, h, 0x81, 0, cs)) {   // WM_NCCREATE: 0 = abort creation
        fprintf(stderr, "[user32] WM_NCCREATE refused\n");
        if (w->sdl) SDL_DestroyWindow(w->sdl);
        w->used = 0;
        gfree(cs);
        ret_std(c, 0, 12);
        return;
    }
    if ((int32_t)send(c, h, 0x01, 0, cs) == -1) {   // WM_CREATE: -1 = abort
        if (w->sdl) SDL_DestroyWindow(w->sdl);
        w->used = 0;
        gfree(cs);
        ret_std(c, 0, 12);
        return;
    }
    gfree(cs);
    send(c, h, 0x05, 0, (uint32_t)(w->h << 16 | (w->w & 0xFFFF)));   // WM_SIZE
    send(c, h, 0x03, 0, (uint32_t)(w->y << 16 | (w->x & 0xFFFF)));   // WM_MOVE
    if (style & 0x10000000) { w->visible = 1; if (w->sdl) SDL_ShowWindow(w->sdl); }   // WS_VISIBLE
    ret_std(c, h, 12);
}

IMP(U, DefWindowProcA) {   // (hwnd, msg, wparam, lparam)
    uint32_t msg = ARG(c, 1), r = 0;
    if (msg == 0x81) r = 1;                                        // WM_NCCREATE: allow
    else if (msg == 0x14) r = 1;                                   // WM_ERASEBKGND: done
    else if (msg == 0x10) {                                        // WM_CLOSE: destroy (sent below by DestroyWindow)
        uint32_t esp = REG(c, R_ESP);
        guest_call(c, dyn_stub(U, "DestroyWindow"), 1, 1, ARG(c, 0));
        REG(c, R_ESP) = esp;
    }
    ret_std(c, r, 4);
}
IMP(U, DestroyWindow) {
    uint32_t h = ARG(c, 0);
    Win *w = win(h);
    if (!w) { ret_std(c, 0, 1); return; }
    send(c, h, 0x02, 0, 0);   // WM_DESTROY
    send(c, h, 0x82, 0, 0);   // WM_NCDESTROY
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    if (h == main_hwnd) main_hwnd = 0;
    w->used = 0;
    ret_std(c, 1, 1);
}

// ---------- the message queue ----------

typedef struct { uint32_t hwnd, msg, wp, lp, time; } Msg;
static Msg queue[1024];
static int q_head, q_len;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static void post(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&q_lock);
    if (q_len < 1024) queue[(q_head + q_len++) % 1024] = (Msg){h, msg, wp, lp, (uint32_t)SDL_GetTicks()};
    pthread_mutex_unlock(&q_lock);
}
IMP(U, PostMessageA) { post(ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3)); ret_std(c, 1, 4); }
IMP(U, SendMessageA) { ret_std(c, send(c, ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3)), 4); }
IMP(U, PostQuitMessage) { post(0, 0x12, ARG(c, 0), 0); ret_std(c, 0, 1); }   // WM_QUIT

static int mouse_x, mouse_y;
static uint32_t mk_flags(void) {   // MK_LBUTTON 1, MK_RBUTTON 2, MK_SHIFT 4, MK_CONTROL 8, MK_MBUTTON 0x10
    SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
    SDL_Keymod m = SDL_GetModState();
    return (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (m & SDL_KMOD_SHIFT ? 4 : 0) |
           (m & SDL_KMOD_CTRL ? 8 : 0) | (b & SDL_BUTTON_MMASK ? 0x10 : 0);
}
static float mouse_rel[3];   // motion since the last DirectInput read (dinput.c)

// SDL's keyboard state. Scripted pad presses (CATH_INPUT) are not mirrored as keys: they reach the game as Steam Input
// actions (steam.c) or XInput, and a mirrored B/BACK = Esc opened the menu on top of the pad's own B (leave seat, Undo).
static const bool *kbd(void) { return SDL_GetKeyboardState(NULL); }

// DirectInput state (dinput.c): DIK code = PC scancode (+0x80 for extended keys)
void input_keys(uint8_t dik[256]) {
    const bool *down = kbd();
    memset(dik, 0, 256);
    for (size_t i = 0; i < NKEYS; i++) if (down[keys[i].sdl]) dik[keys[i].scan | (keys[i].ext ? 0x80 : 0)] = 0x80;
}
const char *input_key_name(int dik) {   // NULL = no such key
    for (size_t i = 0; i < NKEYS; i++) if ((keys[i].scan | (keys[i].ext ? 0x80 : 0)) == dik) return keys[i].name;
    return NULL;
}
void input_mouse(int32_t d[3], uint8_t buttons[8]) {
    SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
    for (int i = 0; i < 3; i++) { d[i] = (int32_t)mouse_rel[i]; mouse_rel[i] -= (float)d[i]; }   // keep the fraction
    memset(buttons, 0, 8);   // DirectInput order: left, right, middle, X1, X2
    const SDL_MouseButtonFlags m[5] = {SDL_BUTTON_LMASK, SDL_BUTTON_RMASK, SDL_BUTTON_MMASK, SDL_BUTTON_X1MASK, SDL_BUTTON_X2MASK};
    for (int i = 0; i < 5; i++) if (b & m[i]) buttons[i] = 0x80;
}

// Who asked us to quit: SDL turns SIGTERM/SIGINT into SDL_EVENT_QUIT; this chained handler remembers the sender so the
// log names it (e.g. Steam closing a game it sees running elsewhere on the account).
static volatile pid_t quit_from;
static struct sigaction prev_term, prev_int;
static void on_quit_signal(int s, siginfo_t *i, void *u) {
    quit_from = i->si_pid;
    struct sigaction *p = s == SIGTERM ? &prev_term : &prev_int;
    if (p->sa_flags & SA_SIGINFO) p->sa_sigaction(s, i, u);
    else if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) p->sa_handler(s);
    else { signal(s, SIG_DFL); raise(s); }
}
static void log_quit(const char *what) {
    char path[64], cmd[256] = "?";
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)quit_from);
    FILE *f = quit_from ? fopen(path, "r") : NULL;
    if (f) { size_t n = fread(cmd, 1, sizeof cmd - 1, f); cmd[n] = 0; for (size_t k = 0; k + 1 < n; k++) if (!cmd[k]) cmd[k] = ' '; fclose(f); }
    if (quit_from) fprintf(stderr, "[user32] quit request (%s): signal from pid %d: %s\n", what, (int)quit_from, cmd);
    else fprintf(stderr, "[user32] quit request (%s): not a signal (window closed by the desktop?)\n", what);
}

static void pump(void) {   // SDL events -> WM_* messages for the main window
    // after SDL_Init, so SDL's own SIGTERM/SIGINT handlers exist to chain to; SDL installs them again whenever one of
    // its subsystems starts (audio comes later), so re-attach whenever ours is no longer the one in place
    struct sigaction cur, sa = {0};
    sa.sa_sigaction = on_quit_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    if (!sigaction(SIGTERM, NULL, &cur) && cur.sa_sigaction != on_quit_signal) sigaction(SIGTERM, &sa, &prev_term);
    if (!sigaction(SIGINT, NULL, &cur) && cur.sa_sigaction != on_quit_signal) sigaction(SIGINT, &sa, &prev_int);
    SDL_Event e;
    while (main_hwnd && SDL_PollEvent(&e)) {
        uint32_t h = main_hwnd;
        switch (e.type) {
        case SDL_EVENT_QUIT: log_quit("SDL_EVENT_QUIT"); post(h, 0x10, 0, 0); break;   // WM_CLOSE
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: log_quit("window close"); post(h, 0x10, 0, 0); break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const Key *k = key_by_sdl(e.key.scancode);
            if (!k) break;
            int down = e.type == SDL_EVENT_KEY_DOWN, alt = (e.key.mod & SDL_KMOD_ALT) != 0;
            uint32_t vk = k->vk;
            if (vk == 0x10) vk = e.key.scancode == SDL_SCANCODE_RSHIFT ? 0xA1 : 0x10;
            uint32_t lp = 1 | (uint32_t)k->scan << 16 | (uint32_t)k->ext << 24 | (alt ? 1u << 29 : 0) |
                          (e.key.repeat || !down ? 1u << 30 : 0) | (down ? 0 : 1u << 31);
            uint32_t msg = (alt || k->vk == 0x79 || k->vk == 0x12) ? (down ? 0x104 : 0x105) : (down ? 0x100 : 0x101);
            post(h, msg, k->vk, lp);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            mouse_x = (int)e.motion.x; mouse_y = (int)e.motion.y;
            mouse_rel[0] += e.motion.xrel; mouse_rel[1] += e.motion.yrel;
            post(h, 0x200, mk_flags(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            uint32_t m = e.button.button == SDL_BUTTON_LEFT ? 0x201 : e.button.button == SDL_BUTTON_RIGHT ? 0x204 : 0x207;
            post(h, down ? m : m + 1, mk_flags(), (uint32_t)((int)e.button.y << 16 | ((int)e.button.x & 0xFFFF)));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            mouse_rel[2] += e.wheel.y * 120;   // DirectInput wheel units = WHEEL_DELTA per notch
            post(h, 0x20A, (uint32_t)((int)(e.wheel.y * 120) << 16) | mk_flags(), (uint32_t)(mouse_y << 16 | (mouse_x & 0xFFFF)));
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            focus_hwnd = h;
            post(h, 0x1C, 1, 0); post(h, 0x06, 1, 0); post(h, 0x07, 0, 0);   // WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            post(h, 0x06, 0, 0); post(h, 0x1C, 0, 0); post(h, 0x08, 0, 0);   // WM_ACTIVATE, WM_ACTIVATEAPP, WM_KILLFOCUS
            break;
        case SDL_EVENT_WINDOW_RESIZED: {
            Win *w = win(h);
            static int logged;   // window diagnosis: sizes the display server reports (first 20)
            if (w && logged++ < 20)
                fprintf(stderr, "[user32] resized %08x to %dx%d (was %dx%d)\n", h, e.window.data1, e.window.data2, w->w, w->h);
            if (w) { w->w = e.window.data1; w->h = e.window.data2; }
            post(h, 0x05, 0, (uint32_t)(e.window.data2 << 16 | (e.window.data1 & 0xFFFF)));   // WM_SIZE, SIZE_RESTORED
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED: post(h, 0x05, 1, 0); break;      // SIZE_MINIMIZED
        case SDL_EVENT_WINDOW_RESTORED: {
            Win *w = win(h);
            if (w) post(h, 0x05, 0, (uint32_t)(w->h << 16 | (w->w & 0xFFFF)));
            break;
        }
        }
    }
}
IMP(U, PeekMessageA) {   // (MSG*, hwnd, min, max, remove)
    uint32_t out = ARG(c, 0), hf = ARG(c, 1), lo = ARG(c, 2), hi = ARG(c, 3), remove = ARG(c, 4) & 1;
    pump();
    pthread_mutex_lock(&q_lock);
    for (int i = 0; i < q_len; i++) {
        Msg *m = &queue[(q_head + i) % 1024];
        if (hf && hf != 0xFFFFFFFF && m->hwnd != hf) continue;
        if ((lo || hi) && (m->msg < lo || m->msg > hi)) continue;
        U32(out) = m->hwnd; U32(out + 4) = m->msg; U32(out + 8) = m->wp; U32(out + 12) = m->lp;
        U32(out + 16) = m->time; U32(out + 20) = (uint32_t)mouse_x; U32(out + 24) = (uint32_t)mouse_y;
        if (remove) {
            for (int j = i; j > 0; j--) queue[(q_head + j) % 1024] = queue[(q_head + j - 1) % 1024];
            q_head = (q_head + 1) % 1024;
            q_len--;
        }
        pthread_mutex_unlock(&q_lock);
        ret_std(c, 1, 5);
        return;
    }
    pthread_mutex_unlock(&q_lock);
    ret_std(c, 0, 5);
}
IMP(U, TranslateMessage) {   // WM_KEYDOWN of a printable key -> WM_CHAR
    uint32_t m = ARG(c, 0), msg = U32(m + 4), vk = U32(m + 8);
    if (msg == 0x100) {
        int shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        int ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = shift ? (int)vk : (int)vk + 32;
        else if ((vk >= '0' && vk <= '9') || vk == 0x20 || vk == 0x0D || vk == 0x08 || vk == 0x09 || vk == 0x1B) ch = (int)vk;
        if (ch) post(U32(m), 0x102, (uint32_t)ch, U32(m + 12));
    }
    ret_std(c, 0, 1);
}
IMP(U, DispatchMessageA) {
    uint32_t m = ARG(c, 0);
    ret_std(c, send(c, U32(m), U32(m + 4), U32(m + 8), U32(m + 12)), 1);
}
IMP(U, TranslateAcceleratorA) { ret_std(c, 0, 3); }

// ---------- window state ----------

IMP(U, ShowWindow) {   // returns whether it was visible
    Win *w = win(ARG(c, 0));
    uint32_t was = w && w->visible;
    if (w) {
        w->visible = ARG(c, 1) != 0;
        if (w->sdl) { if (w->visible) SDL_ShowWindow(w->sdl); else SDL_HideWindow(w->sdl); }
        if (w->sdl && w->visible) { focus_hwnd = ARG(c, 0); post(ARG(c, 0), 0x1C, 1, 0); post(ARG(c, 0), 0x06, 1, 0); }
    }
    ret_std(c, was, 2);
}
IMP(U, UpdateWindow) { ret_std(c, 1, 1); }
IMP(U, IsWindow) { ret_std(c, win(ARG(c, 0)) != NULL, 1); }
IMP(U, IsWindowVisible) { Win *w = win(ARG(c, 0)); ret_std(c, w && w->visible, 1); }
IMP(U, IsIconic) { Win *w = win(ARG(c, 0)); ret_std(c, w && w->sdl && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED), 1); }
IMP(U, EnableWindow) { ret_std(c, 0, 2); }
IMP(U, SetFocus) { uint32_t prev = focus_hwnd; focus_hwnd = ARG(c, 0); ret_std(c, prev, 1); }
IMP(U, GetDesktopWindow) { ret_std(c, HWND_DESKTOP, 0); }
static void set_rect(uint32_t r, int l, int t, int rr, int b) { U32(r) = (uint32_t)l; U32(r + 4) = (uint32_t)t; U32(r + 8) = (uint32_t)rr; U32(r + 12) = (uint32_t)b; }
IMP(U, GetWindowRect) {
    uint32_t h = ARG(c, 0);
    int sw, sh;
    screen_size(&sw, &sh);
    Win *w = win(h);
    if (h == HWND_DESKTOP || !w) set_rect(ARG(c, 1), 0, 0, sw, sh);
    else set_rect(ARG(c, 1), w->x, w->y, w->x + w->w, w->y + w->h);
    ret_std(c, h == HWND_DESKTOP || w, 2);
}
IMP(U, GetClientRect) {
    uint32_t h = ARG(c, 0);
    int sw, sh;
    screen_size(&sw, &sh);
    Win *w = win(h);
    if (h == HWND_DESKTOP || !w) set_rect(ARG(c, 1), 0, 0, sw, sh);
    else set_rect(ARG(c, 1), 0, 0, w->w, w->h);
    ret_std(c, 1, 2);
}
IMP(U, AdjustWindowRect) { ret_std(c, 1, 3); }   // our windows have no frame: client rect = window rect
static void place(CPU *c, uint32_t h, int x, int y, int ww, int hh, int move, int size) {
    WCOUNT(move ? "SetWindowPos/MoveWindow (move)" : "SetWindowPos (no move)");
    Win *w = win(h);
    if (!w) return;
    static int logged;   // window diagnosis: placements that change something (first 40)
    if (((move && (x != w->x || y != w->y)) || (size && (ww != w->w || hh != w->h))) && logged++ < 40)
        fprintf(stderr, "[user32] place %08x pos %d,%d size %dx%d (move %d size %d, was %d,%d %dx%d) from %08x\n", h, x, y,
                ww, hh, move, size, w->x, w->y, w->w, w->h, U32(REG(c, R_ESP)));
    if (move) { w->x = x; w->y = y; if (w->sdl) SDL_SetWindowPosition(w->sdl, x, y); }
    if (size && ww > 0 && hh > 0 && (ww != w->w || hh != w->h)) {
        w->w = ww; w->h = hh;
        // wait until the display server has applied it: else the size it reports for the window's first appearance
        // (the 2x2 it was created at) arrives later as a resize event and shrinks the game's window back (seen at a
        // 1280x800 setting, where gamescope sends no further resize because the window already fills the screen)
        if (w->sdl) { SDL_SetWindowSize(w->sdl, ww, hh); SDL_SyncWindow(w->sdl); }
        send(c, h, 0x05, 0, (uint32_t)(hh << 16 | (ww & 0xFFFF)));
    }
}
IMP(U, MoveWindow) { place(c, ARG(c, 0), (int32_t)ARG(c, 1), (int32_t)ARG(c, 2), (int32_t)ARG(c, 3), (int32_t)ARG(c, 4), 1, 1); ret_std(c, 1, 6); }
IMP(U, SetWindowPos) {   // (hwnd, after, x, y, cx, cy, flags): SWP_NOSIZE 1, SWP_NOMOVE 2, SHOW 0x40, HIDE 0x80
    uint32_t h = ARG(c, 0), fl = ARG(c, 6);
    place(c, h, (int32_t)ARG(c, 2), (int32_t)ARG(c, 3), (int32_t)ARG(c, 4), (int32_t)ARG(c, 5), !(fl & 2), !(fl & 1));
    Win *w = win(h);
    if (w && (fl & 0x40)) { w->visible = 1; if (w->sdl) SDL_ShowWindow(w->sdl); }
    if (w && (fl & 0x80)) { w->visible = 0; if (w->sdl) SDL_HideWindow(w->sdl); }
    ret_std(c, 1, 7);
}
IMP(U, SetWindowTextA) { WCOUNT("SetWindowTextA"); Win *w = win(ARG(c, 0)); if (w && w->sdl) SDL_SetWindowTitle(w->sdl, GSTR(ARG(c, 1))); ret_std(c, 1, 2); }
static uint32_t *long_slot(Win *w, int32_t i) {
    switch (i) {
    case -4: return &w->wndproc;       // GWL_WNDPROC (subclassing)
    case -6: return &w->inst;          // GWL_HINSTANCE
    case -8: return &w->parent;        // GWL_HWNDPARENT
    case -12: return &w->id;           // GWL_ID
    case -16: return &w->style;        // GWL_STYLE
    case -20: return &w->exstyle;      // GWL_EXSTYLE
    case -21: return &w->userdata;     // GWL_USERDATA
    }
    return i >= 0 && i + 4 <= 64 ? (uint32_t *)(w->extra + i) : NULL;
}
IMP(U, GetWindowLongA) {
    Win *w = win(ARG(c, 0));
    uint32_t *p = w ? long_slot(w, (int32_t)ARG(c, 1)) : NULL;
    ret_std(c, p ? *p : 0, 2);
}
IMP(U, SetWindowLongA) {
    Win *w = win(ARG(c, 0));
    uint32_t *p = w ? long_slot(w, (int32_t)ARG(c, 1)) : NULL, old = p ? *p : 0;
    if (p) *p = ARG(c, 2);
    if (w && w->sdl && (int32_t)ARG(c, 1) == -16) {   // style change: borderless (WS_POPUP) vs. windowed
        SDL_SetWindowBordered(w->sdl, !(ARG(c, 2) & 0x80000000u));
    }
    ret_std(c, old, 3);
}
IMP(U, GetWindowPlacement) {   // WINDOWPLACEMENT: length, flags, showCmd, ptMin, ptMax, rcNormal
    Win *w = win(ARG(c, 0));
    uint32_t p = ARG(c, 1);
    memset(GM(p + 4), 0, 40);
    U32(p + 8) = w && w->sdl && (SDL_GetWindowFlags(w->sdl) & SDL_WINDOW_MINIMIZED) ? 2 : 1;
    if (w) set_rect(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
    ret_std(c, w != NULL, 2);
}
IMP(U, GetMonitorInfoW) {   // MONITORINFO: cbSize, rcMonitor, rcWork, dwFlags
    int sw, sh;
    screen_size(&sw, &sh);
    uint32_t p = ARG(c, 1);
    set_rect(p + 4, 0, 0, sw, sh);
    set_rect(p + 20, 0, 0, sw, sh);
    U32(p + 36) = 1;   // MONITORINFOF_PRIMARY
    ret_std(c, 1, 2);
}
IMP(U, GetSystemMetrics) {
    int sw, sh;
    screen_size(&sw, &sh);
    int32_t i = (int32_t)ARG(c, 0);
    uint32_t v = i == 0 || i == 16 || i == 78 ? (uint32_t)sw : i == 1 || i == 17 || i == 79 ? (uint32_t)sh : i == 80 ? 1 : 0;
    ret_std(c, v, 1);   // SM_CXSCREEN/CYSCREEN, full-screen client, virtual screen, monitor count
}

// ---------- keyboard / mouse / cursor ----------

static uint32_t key_state(uint32_t vk) {   // 0x8000 = down
    sdl_up();
    if (vk == 1 || vk == 2 || vk == 4) {
        SDL_MouseButtonFlags b = SDL_GetMouseState(NULL, NULL);
        return (vk == 1 && (b & SDL_BUTTON_LMASK)) || (vk == 2 && (b & SDL_BUTTON_RMASK)) || (vk == 4 && (b & SDL_BUTTON_MMASK)) ? 0x8000 : 0;
    }
    const bool *ks = kbd();
    if (vk == 0xA0) return ks[SDL_SCANCODE_LSHIFT] ? 0x8000 : 0;
    if (vk == 0xA1) return ks[SDL_SCANCODE_RSHIFT] ? 0x8000 : 0;
    if (vk == 0xA2) return ks[SDL_SCANCODE_LCTRL] ? 0x8000 : 0;
    if (vk == 0xA3) return ks[SDL_SCANCODE_RCTRL] ? 0x8000 : 0;
    for (size_t i = 0; i < NKEYS; i++) if (keys[i].vk == vk && ks[keys[i].sdl]) return 0x8000;
    return 0;
}
static uint32_t key_state_logged(uint32_t vk, uint32_t caller) {   // input diagnosis: state changes per key (first 200)
    static uint8_t last[256];
    static int logged;
    uint32_t s = key_state(vk);
    if (vk < 256 && !!s != last[vk] && logged < 200)
        logged++, fprintf(stderr, "[input] key vk %02x %s (from %08x) at %llu ms\n", vk, s ? "down" : "up", caller,
                          (unsigned long long)SDL_GetTicks());
    if (vk < 256) last[vk] = !!s;
    return s;
}
IMP(U, GetAsyncKeyState) { ret_std(c, key_state_logged(ARG(c, 0), U32(REG(c, R_ESP))), 1); }
IMP(U, GetKeyState) { ret_std(c, key_state_logged(ARG(c, 0), U32(REG(c, R_ESP))) ? 0xFF80 : 0, 1); }   // SHORT: high bit = down
IMP(U, keybd_event) { ret_std(c, 0, 4); }
IMP(U, MapVirtualKeyW) {   // (code, type): 0 VK->scan, 1 scan->VK, 2 VK->char, 3 scan->VK (left/right)
    uint32_t code = ARG(c, 0), type = ARG(c, 1), r = 0;
    const Key *k = type == 1 || type == 3 ? key_by_scan(code & 0xFF, (code >> 8) == 0xE0) : key_by_vk(code);
    if (k) r = type == 0 ? k->scan : type == 2 ? (k->vk >= 'A' && k->vk <= 'Z') || (k->vk >= '0' && k->vk <= '9') || k->vk == ' ' ? k->vk : 0 : k->vk;
    ret_std(c, r, 2);
}
IMP(U, GetKeyNameTextA) {   // (lParam, buf, size): scan code in bits 16-23, extended in bit 24
    uint32_t lp = ARG(c, 0), n = ARG(c, 2);
    const Key *k = key_by_scan(lp >> 16 & 0xFF, lp >> 24 & 1);
    const char *s = k ? k->name : "";
    if (n) { strncpy(GSTR(ARG(c, 1)), s, n - 1); GSTR(ARG(c, 1))[n - 1] = 0; }
    ret_std(c, n ? strlen(GSTR(ARG(c, 1))) : 0, 3);
}
IMP(U, GetKeyboardLayoutNameA) { strcpy(GSTR(ARG(c, 0)), "00000409"); ret_std(c, 1, 1); }   // US English
static int cursor_count;
IMP(U, ShowCursor) {
    WCOUNT("ShowCursor");
    cursor_count += ARG(c, 0) ? 1 : -1;
    if (cursor_count >= 0) SDL_ShowCursor(); else SDL_HideCursor();
    ret_std(c, (uint32_t)cursor_count, 1);
}
static void window_origin(int *x, int *y) {
    Win *w = win(main_hwnd);
    *x = w ? w->x : 0;
    *y = w ? w->y : 0;
}
IMP(U, GetCursorPos) {
    WCOUNT("GetCursorPos");
    int ox, oy;
    window_origin(&ox, &oy);
    float x, y;
    sdl_up();
    SDL_GetMouseState(&x, &y);
    U32(ARG(c, 0)) = (uint32_t)((int)x + ox);
    U32(ARG(c, 0) + 4) = (uint32_t)((int)y + oy);
    ret_std(c, 1, 1);
}
// In play the game re-centres the cursor (SetCursorPos) and re-clips it (ClipCursor) ~30 times a second. Each warp
// reaching gamescope (Game Mode) leaked one of its fds: ~4000 a minute, so after ~2 h it ran out (524288) and the
// screen froze. So the display server only hears about real changes: no warp when the pointer is already there,
// no grab call when the grab state is unchanged.
IMP(U, SetCursorPos) {
    WCOUNT("SetCursorPos");
    Win *w = win(main_hwnd);
    int ox, oy;
    window_origin(&ox, &oy);
    float tx = (float)((int32_t)ARG(c, 0) - ox), ty = (float)((int32_t)ARG(c, 1) - oy), mx, my;
    sdl_up();
    SDL_GetMouseState(&mx, &my);
    if (w && w->sdl && ((int)mx != (int)tx || (int)my != (int)ty)) { WCOUNT("SetCursorPos warp"); SDL_WarpMouseInWindow(w->sdl, tx, ty); }
    ret_std(c, 1, 2);
}
IMP(U, ClipCursor) {
    WCOUNT("ClipCursor");
    static int grabbed = -1;
    Win *w = win(main_hwnd);
    int want = ARG(c, 0) != 0;
    if (w && w->sdl && want != grabbed) { SDL_SetWindowMouseGrab(w->sdl, want); grabbed = want; }
    ret_std(c, 1, 1);
}
IMP(U, ClientToScreen) {
    Win *w = win(ARG(c, 0));
    if (w) { U32(ARG(c, 1)) += (uint32_t)w->x; U32(ARG(c, 1) + 4) += (uint32_t)w->y; }
    ret_std(c, 1, 2);
}
IMP(U, ScreenToClient) {
    Win *w = win(ARG(c, 0));
    if (w) { U32(ARG(c, 1)) -= (uint32_t)w->x; U32(ARG(c, 1) + 4) -= (uint32_t)w->y; }
    ret_std(c, 1, 2);
}

// ---------- resources, painting, dialogs, message boxes ----------

static uint32_t fake_handle(void) { static uint32_t n; return 0xC0010000u + 4 * ++n; }
IMP(U, LoadIconA) { ret_std(c, fake_handle(), 2); }
IMP(U, LoadCursorA) { ret_std(c, fake_handle(), 2); }
IMP(U, LoadImageA) { ret_std(c, fake_handle(), 6); }
#define FAKE_DC 0xC0DC0001u
IMP(U, GetDC) { ret_std(c, FAKE_DC, 1); }
IMP(U, ReleaseDC) { ret_std(c, 1, 2); }
IMP(U, BeginPaint) { memset(GM(ARG(c, 1)), 0, 64); U32(ARG(c, 1)) = FAKE_DC; ret_std(c, FAKE_DC, 2); }
IMP(U, EndPaint) { ret_std(c, 1, 2); }
IMP(U, MessageBoxA) {   // (hwnd, text, caption, type): logged; answers OK (or Yes for yes/no boxes)
    fprintf(stderr, "[MessageBox] %s: %s\n", ARG(c, 2) ? GSTR(ARG(c, 2)) : "", ARG(c, 1) ? GSTR(ARG(c, 1)) : "");
    uint32_t type = ARG(c, 3) & 0xF;
    ret_std(c, type == 4 || type == 3 ? 6 : 1, 4);
}
// ponytail: dialogs are not shown; the game only uses them for optional tools/config screens (log if one appears)
IMP(U, CreateDialogParamA) { fprintf(stderr, "[user32] CreateDialogParamA ignored\n"); ret_std(c, 0, 5); }
IMP(U, EndDialog) { ret_std(c, 1, 2); }
IMP(U, GetDlgItem) { ret_std(c, 0, 2); }
IMP(U, SetDlgItemTextA) { ret_std(c, 1, 3); }
IMP(U, SendDlgItemMessageA) { ret_std(c, 0, 5); }
