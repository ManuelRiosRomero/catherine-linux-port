// 16:10 picture (Steam Deck): the game renders its scene for 16:9 and shows it with black bands on a taller screen.
// Two changes make it fill the screen with more view above and below ("vert+") instead:
//  - Gamebryo's NiCamera::SetViewFrustum (0x7cb790), which every camera goes through, gets 16:9 perspective frustums
//    widened vertically to the screen's shape (orthographic 2D and non-16:9 render-to-texture views are left alone);
//  - the presentation quad (0x641bf0) letterboxes using the constant 9/16 at 0xbf3f48 (read only there): it becomes the
//    screen's height/width, so the scene image is drawn over the whole screen (it is made at the screen's size, below).
#include "host.h"
#include <math.h>
#include <string.h>

static float screen_aspect;   // width / height of the screen, 0 = leave the game's 16:9 alone

void aspect_init(int sw, int sh) {   // user32.c, once the screen size is known
    if (sw <= 0 || sh <= 0 || (float)sw / sh > 16.f / 9 - 0.01f) return;   // 16:9 or wider: nothing to do
    screen_aspect = (float)sw / sh;
    float hw = (float)sh / sw;
    memcpy(GM(0xbf3f48), &hw, 4);
    fprintf(stderr, "[aspect] screen %dx%d: 16:9 views widened vertically to %.3f\n", sw, sh, screen_aspect);
}

// Scene images at the screen's size: 0x6413d0 (window/device setup) stores the largest 16:9 box in the window as the
// scene size at 0xcd734c/0xcd7350 (0x641514), then Resets the device (0x641692 -> 0xa26940 -> 0xa25d30 -> Reset at
// 0xa2632b; retries Reset too) and only then builds every scene render target from those two values (main target and
// depth, the half-size set 0x5f758e, the bloom chain 0x5f99f4, the 1.5x target 0x6055eb, 0x605c97, 0x60724f). Called
// from the Reset bridge: a 16:9 height under a taller back buffer becomes the back buffer's height (1280x720 ->
// 1280x800), so the 3D scene is drawn at full resolution instead of stretched. Startup (640x480), device-lost Resets
// (already 800) and 16:9 screens are left alone.
void aspect_scene_size(uint32_t bw, uint32_t bh) {
    if (!screen_aspect || V(GM(0xcd734c), uint32_t) != bw || V(GM(0xcd7350), uint32_t) != bw * 9 / 16 || bh <= bw * 9 / 16) return;
    V(GM(0xcd7350), uint32_t) = bh;
    fprintf(stderr, "[aspect] scene images %ux%u (was %ux%u)\n", bw, bh, bw, bw * 9 / 16);
}

// Movies are drawn stretched over the whole (16:10) scene image: so a 16:9 movie frame of h rows gets
// aspect_movie_rows(h) rows of picture (centred, black above and below) to come out at its own shape with bands, as
// on a 16:9 screen. Returns h when nothing changes.
int aspect_movie_rows(int w, int h) {
    if (!screen_aspect || h <= 0 || fabsf((float)w / h - 16.f / 9) > 0.01f) return h;
    return (int)(h * screen_aspect / (16.f / 9) + 0.5f);
}

// NiCamera::SetViewFrustum(const NiFrustum &f) (thiscall, ret 4). NiFrustum: left, right, top, bottom, near, far, ortho.
// Camera fields: +0xec left, +0xf0 right, +0xf4 top, +0xf8 bottom, +0xfc near (clamped), +0x100 far, +0x104 ortho,
// near clamp = max(near, far / [+0x10c], [+0x108]) as in the original.
NATIVE(0x7cb790, NiCamera_SetViewFrustum) {
    uint32_t cam = REG(c, R_ECX), f = ARG(c, 0);
    float l = V(GM(f), float), r = V(GM(f + 4), float), t = V(GM(f + 8), float), b = V(GM(f + 12), float);
    float n = V(GM(f + 16), float), far = V(GM(f + 20), float);
    uint8_t ortho = V(GM(f + 24), uint8_t);
    if (screen_aspect && !ortho && t - b > 0 && fabsf((r - l) / (t - b) - 16.f / 9) < 0.01f) {
        float s = 16.f / 9 / screen_aspect;
        t *= s, b *= s;
    }
    float nc = n, q = far / V(GM(cam + 0x10c), float), m = V(GM(cam + 0x108), float);
    if (q > nc) nc = q;
    if (m > nc) nc = m;
    V(GM(cam + 0xfc), float) = nc;
    V(GM(cam + 0xec), float) = l, V(GM(cam + 0xf0), float) = r, V(GM(cam + 0xf4), float) = t, V(GM(cam + 0xf8), float) = b;
    V(GM(cam + 0x100), float) = far;
    V(GM(cam + 0x104), uint8_t) = ortho;
    ret_std(c, (U32(f) & 0xFFFFFF00u) | ortho, 1);   // EAX as the original leaves it (AL = ortho over the left value)
}
