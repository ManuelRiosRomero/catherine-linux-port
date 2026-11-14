// XInput 1.3 on SDL3 gamepads. Player i = the i-th connected gamepad. SDL's event pump (user32.c) keeps their state fresh.
#include "host.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XI "xinput1_3.dll"
#define NOT_CONNECTED 1167   // ERROR_DEVICE_NOT_CONNECTED

static SDL_Gamepad *pads[4];
static uint32_t packet[4];
static uint8_t last[4][12];

static SDL_Gamepad *pad(uint32_t user) {
    static uint64_t next_scan;
    if (user >= 4) return NULL;
    if (pads[user] && SDL_GamepadConnected(pads[user])) return pads[user];
    if (SDL_GetTicks() < next_scan) return NULL;           // rescan for hot-plugged pads at most once a second
    next_scan = SDL_GetTicks() + 1000;
    SDL_UpdateGamepads();   // HIDAPI devices (the Deck's own controls) only appear after an update
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < 4; i++) {
        if (pads[i] && !SDL_GamepadConnected(pads[i])) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
        if (!pads[i] && i < n) {
            pads[i] = SDL_OpenGamepad(ids[i]);
            if (pads[i]) fprintf(stderr, "[xinput] player %d = %s\n", i, SDL_GetGamepadName(pads[i]));
        }
    }
    SDL_free(ids);
    return pads[user];
}

static int16_t axis(SDL_Gamepad *p, SDL_GamepadAxis a, int flip) {
    int v = SDL_GetGamepadAxis(p, a);
    if (flip) v = -v - 1;                                    // SDL: down = +; XInput: up = + (keeps the range)
    return (int16_t)v;
}

// Test automation, on top of the real pad (player 0, and Steam Input: steam.c). A press is "NAME[+NAME...][/ms]", held
// ms (default 150); names: A B X Y UP DOWN LEFT RIGHT START BACK LB RB; "A+DOWN/600" holds both (pull a block).
//   CATH_INPUT="40:A 42:DOWN 44.5:START/800"  presses at those times (seconds since start)
//   CATH_INPUT_FILE=<path>  live control: presses appended to the file run one after another as soon as they are read
//                           (e.g. echo "UP/700 UP/700 A+DOWN/600 WAIT/500" >> file); WAIT/ms only waits
#define MAX_EV 4096
static struct { uint64_t ms, hold; uint16_t b; } ev[MAX_EV];
static int n_ev;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static int press(char *t, uint64_t *hold) {   // "UP+A/700" -> button bits (0 = WAIT or unknown), *hold = ms
    static const struct { const char *name; uint16_t x; } names[] = {{"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000},
        {"Y", 0x8000}, {"UP", 0x1}, {"DOWN", 0x2}, {"LEFT", 0x4}, {"RIGHT", 0x8}, {"START", 0x10}, {"BACK", 0x20},
        {"LB", 0x100}, {"RB", 0x200}};
    char *slash = strchr(t, '/'), *save = NULL;
    *hold = slash ? strtoull(slash + 1, NULL, 10) : 150;
    if (slash) *slash = 0;
    int b = 0;
    for (char *k = strtok_r(t, "+", &save); k; k = strtok_r(NULL, "+", &save))
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(k, names[i].name)) b |= names[i].x;
    return b;
}
static void live_poll(uint64_t now) {   // new complete lines of CATH_INPUT_FILE -> queued presses
    static const char *path;
    static long off;
    static uint64_t next_check, cursor;
    if (!path && !(path = getenv("CATH_INPUT_FILE"))) path = "";
    if (!*path || now < next_check) return;
    next_check = now + 50;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    if (fseek(f, off, SEEK_SET) == 0)
        while (fgets(line, sizeof line, f) && strchr(line, '\n')) {
            off = ftell(f);
            char *save = NULL;
            for (char *t = strtok_r(line, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
                uint64_t hold, start = cursor > now ? cursor : now;
                int b = press(t, &hold);
                if (b && n_ev < MAX_EV) ev[n_ev].ms = start, ev[n_ev].hold = hold, ev[n_ev++].b = (uint16_t)b;
                cursor = start + hold + (b ? 120 : 0);   // a short release between presses
            }
        }
    fclose(f);
}
uint16_t input_scripted(void) {
    static int parsed;
    pthread_mutex_lock(&ev_lock);
    if (!parsed) {
        parsed = 1;
        char *s = getenv("CATH_INPUT") ? strdup(getenv("CATH_INPUT")) : NULL, *save = NULL;
        for (char *t = s ? strtok_r(s, " ", &save) : NULL; t && n_ev < MAX_EV; t = strtok_r(NULL, " ", &save)) {
            char *colon = strchr(t, ':');
            uint64_t hold;
            int b = colon ? press(colon + 1, &hold) : 0;
            if (b) ev[n_ev].ms = (uint64_t)(atof(t) * 1000), ev[n_ev].hold = hold, ev[n_ev++].b = (uint16_t)b;
        }
        free(s);
    }
    uint64_t now = SDL_GetTicks();
    live_poll(now);
    uint16_t b = 0;
    for (int i = 0; i < n_ev; i++)
        if (now >= ev[i].ms && now < ev[i].ms + ev[i].hold) b |= ev[i].b;
    pthread_mutex_unlock(&ev_lock);
    return b;
}

// XINPUT_GAMEPAD (12 bytes): buttons, left/right trigger (0-255), LX, LY, RX, RY
static void read_pad(SDL_Gamepad *p, uint8_t g[12]) {
    static const struct { SDL_GamepadButton b; uint16_t x; } map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8}, {SDL_GAMEPAD_BUTTON_START, 0x10}, {SDL_GAMEPAD_BUTTON_BACK, 0x20},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000}, {SDL_GAMEPAD_BUTTON_WEST, 0x4000},
        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    uint16_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GetGamepadButton(p, map[i].b)) b |= map[i].x;
    if (p == pads[0]) b |= input_scripted();
    int16_t s[4] = {axis(p, SDL_GAMEPAD_AXIS_LEFTX, 0), axis(p, SDL_GAMEPAD_AXIS_LEFTY, 1),
                    axis(p, SDL_GAMEPAD_AXIS_RIGHTX, 0), axis(p, SDL_GAMEPAD_AXIS_RIGHTY, 1)};
    memcpy(g, &b, 2);
    g[2] = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
    g[3] = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
    memcpy(g + 4, s, 8);
}

IMP(XI, XInputGetState) {   // (user, XINPUT_STATE*: packet number + XINPUT_GAMEPAD)
    uint32_t u = ARG(c, 0), out = ARG(c, 1);
    SDL_Gamepad *p = steam_input_pads ? NULL : pad(u);   // see host.h: one source per controller, like Windows
    if (!p) { ret_std(c, NOT_CONNECTED, 2); return; }
    uint8_t g[12];
    read_pad(p, g);
    if (memcmp(g, last[u], 12)) {   // the packet number changes with the state
        static int logged;   // input diagnosis: A button edges as XInput reports them (first 200)
        if (((g[1] ^ last[u][1]) & 0x10) && logged++ < 200)
            fprintf(stderr, "[input] xinput A %d at %llu ms\n", (g[1] >> 4) & 1, (unsigned long long)SDL_GetTicks());
        memcpy(last[u], g, 12);
        packet[u]++;
    }
    U32(out) = packet[u];
    memcpy(GM(out + 4), g, 12);
    ret_std(c, 0, 2);
}
IMP(XI, XInputSetState) {   // (user, XINPUT_VIBRATION*: left/right motor 0-65535); XInput keeps it until changed
    SDL_Gamepad *p = pad(ARG(c, 0));
    if (!p) { ret_std(c, NOT_CONNECTED, 2); return; }
    uint16_t lo = V(GM(ARG(c, 1)), uint16_t), hi = V(GM(ARG(c, 1) + 2), uint16_t);
    SDL_RumbleGamepad(p, lo, hi, lo || hi ? 10000 : 0);   // ponytail: 10 s cap; games refresh vibration far more often
    ret_std(c, 0, 2);
}
IMP(XI, XInputGetCapabilities) {   // (user, flags, XINPUT_CAPABILITIES*: type, subtype, flags, gamepad, vibration)
    uint32_t out = ARG(c, 2);
    if (!pad(ARG(c, 0))) { ret_std(c, NOT_CONNECTED, 3); return; }
    memset(GM(out), 0, 20);
    GM(out)[0] = 1;                    // XINPUT_DEVTYPE_GAMEPAD
    GM(out)[1] = 1;                    // XINPUT_DEVSUBTYPE_GAMEPAD
    V(GM(out + 4), uint16_t) = 0xF3FF; // every button
    GM(out + 6)[0] = GM(out + 7)[0] = 0xFF;
    memset(GM(out + 8), 0xFF, 8);      // full stick resolution
    U32(out + 16) = 0xFFFFFFFF;        // both motors
    ret_std(c, 0, 3);
}
IMP(XI, XInputEnable) { ret_std(c, 0, 1); }
// (user, render GUID*, capture GUID*): no headset = GUID_NULL for both
IMP(XI, XInputGetDSoundAudioDeviceGuids) {
    if (!pad(ARG(c, 0))) { ret_std(c, NOT_CONNECTED, 3); return; }
    memset(GM(ARG(c, 1)), 0, 16); memset(GM(ARG(c, 2)), 0, 16);
    ret_std(c, 0, 3);
}
IMP(XI, XInputGetBatteryInformation) {   // (user, devtype, {type, level}): wired, full
    if (!pad(ARG(c, 0))) { ret_std(c, NOT_CONNECTED, 3); return; }
    GM(ARG(c, 2))[0] = 1; GM(ARG(c, 2))[1] = 3;
    ret_std(c, 0, 3);
}
IMP(XI, XInputGetKeystroke) { ret_std(c, pad(ARG(c, 0)) ? 4306 : NOT_CONNECTED, 3); }   // ERROR_EMPTY: no keystroke queued
// The gamepad subsystem must start before any game code: started later (after the in-process Steam client is up),
// SDL finds no devices on the Deck.
__attribute__((constructor)) static void pad_init(void) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "[xinput] SDL gamepad init: %s\n", SDL_GetError());
}
