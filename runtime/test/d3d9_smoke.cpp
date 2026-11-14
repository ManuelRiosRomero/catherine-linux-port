// Graphics-stack check: SDL3 window + dxvk-native D3D9 device, clear to purple for ~3 s, then exit 0.
// Proves Vulkan + dxvk + the window system work on the target before the guest D3D9 bridge is involved.
#include <SDL3/SDL.h>
#include <d3d9.h>
#include <cstdio>

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *w = SDL_CreateWindow("d3d9 smoke", 1280, 800, SDL_WINDOW_VULKAN);
    if (!w) { std::printf("SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { std::printf("Direct3DCreate9 failed\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id = {};
    d3d->GetAdapterIdentifier(0, 0, &id);
    std::printf("adapter: %s\n", id.Description);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 800;
    pp.hDeviceWindow = reinterpret_cast<HWND>(w);   // dxvk-native: the HWND is the SDL window
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, pp.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { std::printf("CreateDevice failed: %08x\n", (unsigned)hr); return 1; }
    Uint64 end = SDL_GetTicks() + 3000;
    int frames = 0;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(120, 40, 160), 1.0f, 0);
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        frames++;
    }
    std::printf("presented %d frames OK\n", frames);
    dev->Release();
    d3d->Release();
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
