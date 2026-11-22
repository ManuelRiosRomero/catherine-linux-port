// 16:10 picture (Steam Deck): the game renders its scene for 16:9 and shows it with black bands on a taller screen.
// Two changes make it fill the screen with more view above and below ("vert+") instead:
//  - Gamebryo's NiCamera::SetViewFrustum (0x7cb790), which every camera goes through, gets 16:9 perspective frustums
//    widened vertically to the screen's shape (orthographic 2D and non-16:9 render-to-texture views are left alone);
//  - the presentation quad (0x641bf0) letterboxes using the constant 9/16 at 0xbf3f48 (read only there): it becomes the
//    screen's height/width, so the scene image is drawn over the whole screen (it is made at the screen's size, below).
// The 2D UI (laid out on a 1280x720 canvas) is drawn into a centred 16:9 band so it keeps its shape: a projection scale
// for the sprite layers, full-screen fades and backdrops pushed back to the edges, movies and 3D-anchored markers fixed.
// Design: workflows wf_f1c56248-a8c (scene) and wf_b193a078-045 (UI), HANDOVER session 14g.
#include "host.h"
#include <math.h>
#include <string.h>

static float screen_aspect;   // width / height of the screen, 0 = leave the game's 16:9 alone
static int full_scene = 1;    // scene images at the screen's size (else 16:9-sized, stretched to the screen)
static float ui_f;            // 2D UI band: share of the target height the 16:9 UI canvas gets (0.9 on 16:10), 0 = off

// The player picks the look in Steam's Launch Options (e.g. CATH_ASPECT=16:9 .../catherine-launch.sh %command%):
// CATH_ASPECT unset or "fill" = 16:10 at full resolution, "fill720" = 16:10 from a 1280x720 scene image, "16:9" = the
// original bands; CATH_UI unset or "true" = 2D UI at its true 16:9 shape in a centred band, "stretch" = 2D UI drawn
// over the whole screen (~11% taller).
void aspect_init(int sw, int sh) {   // user32.c, once the screen size is known
    const char *mode = getenv("CATH_ASPECT");
    if (mode && !strcmp(mode, "16:9")) { fprintf(stderr, "[aspect] CATH_ASPECT=16:9: original 16:9 picture\n"); return; }
    if (mode && strcmp(mode, "fill") && strcmp(mode, "fill720")) fprintf(stderr, "[aspect] CATH_ASPECT=%s unknown: using fill\n", mode);
    full_scene = !(mode && !strcmp(mode, "fill720"));
    if (sw <= 0 || sh <= 0 || (float)sw / sh > 16.f / 9 - 0.01f) return;   // 16:9 or wider: nothing to do
    screen_aspect = (float)sw / sh;
    const char *ui = getenv("CATH_UI");
    if (ui && strcmp(ui, "true") && strcmp(ui, "stretch")) fprintf(stderr, "[aspect] CATH_UI=%s unknown: using true\n", ui);
    if (!(ui && !strcmp(ui, "stretch"))) ui_f = screen_aspect * 9 / 16;
    fprintf(stderr, "[aspect] 2D UI: %s\n", ui_f ? "true 16:9 shape" : "stretched");
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
    if (!screen_aspect || !full_scene || V(GM(0xcd734c), uint32_t) != bw || V(GM(0xcd7350), uint32_t) != bw * 9 / 16 || bh <= bw * 9 / 16) return;
    V(GM(0xcd7350), uint32_t) = bh;
    fprintf(stderr, "[aspect] scene images %ux%u (was %ux%u)\n", bw, bh, bw, bw * 9 / 16);
}

// 2D UI at its true 16:9 shape: the 6 sprite layer clicks (Aw2DPrimitiveRenderClick, vtable 0xb855f8, render 0x5f22a0)
// set their camera with NiDX9Renderer::Do_SetScreenSpaceCameraData (0xa29840), whose PROJECTION SetTransform at
// 0xa29b53 (returns to 0xa29b59) passes the renderer's own matrix (+0x830; _22 = 2.0 at +0x844, rewritten from an
// immediate on every call). _22 *= f puts the 1280x720 canvas in the centred 16:9 band of the target (the view matrix
// already centres y); viewport and half-pixel terms stay. Written into the renderer's matrix on purpose, not a copy:
// the shader constant code reads +0x830 too (0xa36178, 0xa39226). Only for targets that end up stretched over the
// screen (back buffer, scene image, 1.5x scene image); other sizes are logged and left alone.
void aspect_ui_projection(CPU *c) {   // d3d9_bridge.cpp SetTransform wrapper, before forwarding
    if (!ui_f || ARG(c, 1) != 3 || U32(REG(c, R_ESP)) != 0xa29b59) return;   // D3DTS_PROJECTION from 0xa29b53
    uint32_t ebp = REG(c, R_EBP), r = REG(c, R_EDI), m = ARG(c, 2);
    if (U32(ebp + 4) != 0x5f2342 || !r || U32(r - 0x14) != 0xb855f8 || m != REG(c, R_ESI) + 0x830) return;
    if (V(GM(r), float) != 0 || V(GM(r + 4), float) != 1 || V(GM(r + 8), float) != 1 || V(GM(r + 12), float) != 0) return;   // full-target clicks only
    uint32_t w = (uint32_t)V(GM(ebp - 0x20), float), h = (uint32_t)V(GM(ebp - 0x24), float);
    uint32_t sw = V(GM(0xcd734c), uint32_t), sh = V(GM(0xcd7350), uint32_t), sc = V(GM(0xcd7354), uint32_t), sr = V(GM(0xcd7358), uint32_t);
    if ((w == sw && h == sh) || (w == sw * 3 / 2 && h == sh * 3 / 2) || (w == sc && h == sr)) { V(GM(m + 0x14), float) = 2.f * ui_f; return; }
    static int logged;   // an unexpected target: left alone (seen: none, all 6 layers draw into screen-stretched images)
    if (logged++ < 4) fprintf(stderr, "[aspect] 2D UI layer click into a %ux%u target: left unbanded\n", w, h);
}

// Movies drawn by their own full-canvas sprite (AwMovie +0x24 != 0: 0x5f0910 from 0x6a5161, layer 0 or 5) are
// stretched over the whole (16:10) scene image, also under the UI band (the whole-canvas rule below): so a 16:9 frame
// of h rows gets aspect_movie_rows rows of picture (centred, black above and below) to come out at its own shape with
// bands, as on a 16:9 screen. With the UI band on, the others (own_sprite 0: tutorial window in layer 1, movie
// textures on 3D meshes) already keep their shape. Returns h when nothing changes.
int aspect_movie_rows(int w, int h, int own_sprite) {
    if (!screen_aspect || h <= 0 || fabsf((float)w / h - 16.f / 9) > 0.01f || (ui_f && !own_sprite)) return h;
    return (int)(h * screen_aspect / (16.f / 9) + 0.5f);
}

// Full-screen 2D under the UI band: the sprite allocator 0x5f05c0 batches consecutive sprites of one layer and texture
// into one NiScreenElements, so a fade and a menu box can share a draw call; only the vertex setters see each polygon.
// They are native (tools/recomp/native.txt) and do the original work, then: in a banded sprite layer, flat-colour
// vertices on or beyond the top/bottom canvas edge move out to the screen edge (fades, dims, bars, the 0x5c9280 tile
// wipe reach the margins), textured polygons over the whole canvas (backdrops, the full-screen movie sprite) are drawn
// full height again, and polygons lying entirely above or below the canvas (parked, invisible in the original) are
// moved far off screen so they cannot show in the margins.
#define EDGE_Y (0.5f / 720)    // half a canvas pixel
#define EDGE_X (0.5f / 1280)
static float xmmf(CPU *c, int i) { float f; memcpy(&f, &REG(c, 0x1200 + 0x40 * i), 4); return f; }   // XMMi lane 0
static float unband(float y) { return 0.5f + (y - 0.5f) / ui_f; }   // canvas y whose banded image lands where y was before
static uint32_t poly_of(uint32_t e, int32_t h) {   // polygon handle -> {u16 vertex count, u16 first vertex}, 0 = invalid
    if (h < 0 || h >= V(GM(e + 0x130), uint16_t)) return 0;
    uint16_t p = V(GM(U32(e + 0x12c) + 2 * h), uint16_t);
    return p == 0xffff ? 0 : U32(e + 0x128) + 8 * p;
}
static uint32_t lock_pos(CPU *c, uint32_t e) { REG(c, R_ECX) = e; return guest_call(c, 0x87ead0, 1, 1, 0); }
static void unlock_pos(CPU *c, uint32_t e) { REG(c, R_ECX) = e; guest_call(c, 0x87ed60, 1, 1, 0); V(GM(e + 0x144), uint8_t) = 1; }
static int sprite_layer(uint32_t e, uint32_t ret, uint32_t *tex) {   // banded sprite layer of e (+ its texture), -1 = not a sprite
    *tex = 0;
    if (ret == 0x5c9672) return 0;   // 0x5c9280 tile wipe: flat tiles in layer 0 (0x5c96b0 / 0x5c97a3)
    uint32_t app = U32(0xd3c25c);
    if (!app || U32(e) != 0xb802a8) return -1;
    for (int k = 0; k < 6; k++) {
        if (U32(0xd5aea8 + 4 * k) != e) continue;   // 0x5f05c0: last sprite element of layer k
        uint32_t click = U32(app + 0xb0c0 + 4 * k);
        for (uint32_t n = click ? U32(click + 0x54) : 0; n; n = U32(n))   // and in layer k's click list (next +0, element +8)
            if (U32(n + 8) == e) { *tex = U32(0xd5aec0 + 4 * k); return k; }   // texture of this batch
    }
    return -1;
}
static void full_screen_2d(uint32_t e, uint32_t ret, uint32_t v, int n, int whole) {
    if (!ui_f) return;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;   // cheap geometry first: most calls (text glyphs) stop here
    for (int i = 0; i < n; i++) {
        float x = V(GM(v + 12 * i), float), y = V(GM(v + 12 * i + 4), float);
        x0 = fminf(x0, x), x1 = fmaxf(x1, x), y0 = fminf(y0, y), y1 = fmaxf(y1, y);
    }
    int outside = n > 1 && (y0 >= 1 || y1 <= 0);
    if (!outside && y0 > EDGE_Y && y1 < 1 - EDGE_Y) return;
    uint32_t tex;
    int k = sprite_layer(e, ret, &tex);
    if (k < 0) return;
    if (outside) {
        for (int i = 0; i < n; i++) V(GM(v + 12 * i + 4), float) = y0 >= 1 ? 4.f : -3.f;
    } else if (tex) {
        if (!whole || x0 > EDGE_X || x1 < 1 - EDGE_X || y0 > EDGE_Y || y1 < 1 - EDGE_Y) return;
        for (int i = 0; i < n; i++) V(GM(v + 12 * i + 4), float) = unband(V(GM(v + 12 * i + 4), float));
    } else {
        for (int i = 0; i < n; i++) {
            float *y = (float *)GM(v + 12 * i + 4);
            if (*y <= EDGE_Y || *y >= 1 - EDGE_Y) *y = unband(*y);
        }
    }
}
// NiScreenElements::SetRectangle(int poly, x XMM2, y XMM3, float w, float h), thiscall, ret 0xc: 4-vertex polygons only,
// vertices (x,y) (x,y+h) (x+w,y+h) (x+w,y), z = 0; lock 0x87ead0 / unlock 0x87ed60, then [e+0x144] = 1; AL = done.
NATIVE(0x87fac0, NiScreenElements_SetRectangle) {
    uint32_t e = REG(c, R_ECX), esp = REG(c, R_ESP), ret = U32(esp), pl = poly_of(e, (int32_t)ARG(c, 0));
    float x = xmmf(c, 2), y = xmmf(c, 3), w = V(GM(esp + 8), float), h = V(GM(esp + 12), float);
    if (!pl || V(GM(pl), uint16_t) != 4) { ret_std(c, 0, 3); return; }
    uint32_t v = lock_pos(c, e) + 12 * V(GM(pl + 2), uint16_t);
    float r = x + w, b = y + h, q[4][2] = {{x, y}, {x, b}, {r, b}, {r, y}};
    for (int i = 0; i < 4; i++) V(GM(v + 12 * i), float) = q[i][0], V(GM(v + 12 * i + 4), float) = q[i][1], U32(v + 12 * i + 8) = 0;
    full_screen_2d(e, ret, v, 4, 1);
    unlock_pos(c, e);
    ret_std(c, 1, 3);
}
// NiScreenElements::SetVertices(int poly, const NiPoint2 *pts), thiscall, ret 8: all vertices of the polygon.
NATIVE(0x87f9f0, NiScreenElements_SetVertices) {
    uint32_t e = REG(c, R_ECX), ret = U32(REG(c, R_ESP)), pl = poly_of(e, (int32_t)ARG(c, 0)), pts = ARG(c, 1);
    if (!pl || !pts) { ret_std(c, 0, 2); return; }
    int n = V(GM(pl), uint16_t);
    uint32_t v = lock_pos(c, e) + 12 * V(GM(pl + 2), uint16_t);
    for (int i = 0; i < n; i++) U32(v + 12 * i) = U32(pts + 8 * i), U32(v + 12 * i + 4) = U32(pts + 8 * i + 4), U32(v + 12 * i + 8) = 0;
    full_screen_2d(e, ret, v, n, 1);
    unlock_pos(c, e);
    ret_std(c, 1, 2);
}
// NiScreenElements::SetVertex(int poly, int vertex, const NiPoint2 *pt), thiscall, ret 0xc: one vertex.
NATIVE(0x87f960, NiScreenElements_SetVertex) {
    uint32_t e = REG(c, R_ECX), ret = U32(REG(c, R_ESP)), pl = poly_of(e, (int32_t)ARG(c, 0)), pt = ARG(c, 2);
    int32_t i = (int32_t)ARG(c, 1);
    if (!pl || i < 0 || i >= V(GM(pl), uint16_t)) { ret_std(c, 0, 3); return; }
    uint32_t v = lock_pos(c, e) + 12 * (V(GM(pl + 2), uint16_t) + i);
    U32(v) = U32(pt), U32(v + 4) = U32(pt + 4), U32(v + 8) = 0;
    full_screen_2d(e, ret, v, 1, 0);   // one vertex: the flat edge rule only
    unlock_pos(c, e);
    ret_std(c, 1, 3);
}

// Markers over 3D under the UI band: NiCamera::WorldPtToScreenPt (0x7cbe40: pt, &x, &y, tol) and its depth variant
// (0x7cc010: pt, &x, &y, &z, tol) give fractions of the full (16:10) camera port; the 9 callers that turn the result into
// 2D UI coordinates (talk/attach prompts 0x5e3b6d 0x5ed3c1, puzzle popups and icons ...) get y moved into the 16:9 band
// (y' = cy + (y - cy) / f, the original 16:9 game's value). Others (radial-blur centres 0x709c9a 0x758f47, visibility
// tests) keep the original result. Copies of the originals in their order of float operations and memory accesses
// (outputs are written, then read back); the on-screen result is judged before the remap; XMM registers untouched.
static int ui_marker_site(uint32_t ret) {
    static const uint32_t s[] = {0x5e3b6d, 0x5ed3c1, 0x64fcea, 0x6cf013, 0x6d8547, 0x6e38f2, 0x6e3dd4, 0x6e4f11, 0x795b10};
    for (unsigned i = 0; i < sizeof s / sizeof *s; i++) if (s[i] == ret) return 1;
    return 0;
}
#define CF(o) V(GM(cam + (o)), float)
#define FM(a) V(GM(a), float)
static void world_to_screen(CPU *c, int depth) {
    uint32_t cam = REG(c, R_ECX), ret = U32(REG(c, R_ESP)), p = ARG(c, 0), px = ARG(c, 1), py = ARG(c, 2);
    uint32_t pz = depth ? ARG(c, 3) : 0, tb = ARG(c, depth ? 4 : 3), out = depth ? pz : py;
    float tol;
    memcpy(&tol, &tb, 4);
    if (!(tol > 0.f)) tol = 0.f;
    float X = FM(p), Y = FM(p + 4), Z = FM(p + 8);
    float w = CF(0xdc) * X + CF(0xe0) * Y + CF(0xe4) * Z + CF(0xe8);
    if (!(w > tol)) {   // behind the camera
        U32(px) = U32(py) = 0x7f7fffff;
        if (depth) U32(pz) = 0x7f7fffff;
        ret_std(c, out & 0xFFFFFF00u, depth ? 5 : 4);
        return;
    }
    float inv = 1.f / w;
    FM(px) = CF(0xac) * X + CF(0xb0) * Y + CF(0xb4) * Z + CF(0xb8);
    FM(py) = CF(0xbc) * FM(p) + CF(0xc0) * FM(p + 4) + CF(0xc4) * FM(p + 8) + CF(0xc8);
    FM(px) = FM(px) * inv;
    FM(py) = FM(py) * inv;
    FM(px) = (CF(0x114) - CF(0x110)) * 0.5f * FM(px);
    FM(py) = (CF(0x118) - CF(0x11c)) * 0.5f * FM(py);
    FM(px) = (CF(0x110) + CF(0x114)) * 0.5f + FM(px);
    FM(py) = (CF(0x11c) + CF(0x118)) * 0.5f + FM(py);
    if (depth) FM(pz) = (CF(0xcc) * FM(p) + CF(0xd0) * FM(p + 4) + CF(0xd4) * FM(p + 8) + CF(0xd8)) * inv;
    float x = FM(px), y = FM(py);
    uint32_t r = x >= CF(0x110) && CF(0x114) >= x && y >= CF(0x11c) && CF(0x118) >= y;   // on the real screen
    if (ui_f && ui_marker_site(ret)) {
        float cy = (CF(0x11c) + CF(0x118)) * 0.5f;
        FM(py) = cy + (y - cy) / ui_f;
    }
    ret_std(c, (out & 0xFFFFFF00u) | r, depth ? 5 : 4);   // EAX as the original leaves it: ptr y (or z), AL = result
}
NATIVE(0x7cbe40, NiCamera_WorldPtToScreenPt) { world_to_screen(c, 0); }
NATIVE(0x7cc010, NiCamera_WorldPtToScreenPt3) { world_to_screen(c, 1); }

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
