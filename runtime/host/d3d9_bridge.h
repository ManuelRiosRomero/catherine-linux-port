// Shared by the generated D3D9 forwarders (d3d9_gen.cpp, from tools/recomp/gen_d3d9.py) and d3d9_bridge.cpp.
#pragma once
#include <d3d9.h>
#include <cstring>
extern "C" {
#include "host.h"
struct SDL_Window;
SDL_Window *hwnd_window(uint32_t hwnd);   // user32.c: the SDL window a guest HWND renders into (walks to its top level)
}

// guest <-> native objects
void *nat(uint32_t gobj);                  // guest wrapper -> native dxvk object (NULL stays NULL)
uint32_t wrap(void *native, int type);     // native -> guest wrapper (same native object -> same wrapper)
static inline void *hptr(uint32_t a) { return a ? (void *)GM(a) : nullptr; }
HWND native_hwnd(uint32_t guest_hwnd);

// generated tables
extern const char *const iface_names[];
extern const int iface_count;
extern const Fn *const iface_methods[];
extern const char *const *const iface_method_names[];
extern const int iface_method_count[];
int iface_type(const char *name);

// hand-written methods (d3d9_bridge.cpp); type = which interface the call came through
void special_QueryInterface(CPU *c, int type);
void special_Release(CPU *c, int type);
void special_CreateDevice(CPU *c, int type);
void special_CreateDeviceEx(CPU *c, int type);
void special_Reset(CPU *c, int type);
void special_ResetEx(CPU *c, int type);
void special_CreateAdditionalSwapChain(CPU *c, int type);
void special_GetPresentParameters(CPU *c, int type);
void special_GetCreationParameters(CPU *c, int type);
void special_LockRect(CPU *c, int type);
void special_UnlockRect(CPU *c, int type);
void special_Lock(CPU *c, int type);
void special_Unlock(CPU *c, int type);
void special_LockBox(CPU *c, int type);
void special_UnlockBox(CPU *c, int type);
void special_GetDC(CPU *c, int type);
void special_ReleaseDC(CPU *c, int type);
void special_GetAdapterMonitor(CPU *c, int type);
void special_GetContainer(CPU *c, int type);
