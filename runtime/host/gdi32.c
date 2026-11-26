// GDI32: the game's glyph rasterizer path (awfont_win32.cpp) on FreeType.
// It loads data/font/*.ttf with AddFontMemResourceEx, CreateFontA's them by family name ("Jenkins v2.0", "Chivo"),
// draws one glyph at a time white-on-black into a memory DC (FillRect, TextOutW), measures it (GetCharABCWidthsFloatW,
// GetTextMetricsW) and reads the bitmap back as 32-bit top-down pixels (GetDIBits) into a texture's alpha channel.
// Objects (DCs, bitmaps, fonts, brushes) live in one handle table; stock objects are fixed handles outside it.
#include "host.h"
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#define G "GDI32.dll"
#define STOCK(i) (0xC0D00000u + 4 * (i))   // GetStockObject(i)
#define STOCK_BITMAP 0xC0D0F000u            // the 1x1 bitmap a new memory DC starts with
#define BASE 0xC0D10000u
#define NOBJ 4096                           // ponytail: fixed table, grow it if the game ever runs out

enum { O_FREE, O_DC, O_BMP, O_FONT, O_BRUSH };
typedef struct {
    int type;
    uint32_t bmp, font, text_color;   // DC: selected handles, COLORREF 0x00BBGGRR
    int w, h; uint32_t *px;           // bitmap: 0x00RRGGBB per pixel, top row first (DIB byte order B,G,R,0)
    FT_Face face; FT_F26Dot6 em;      // font: face + em size in 26.6 pixels
    int ascent, descent;              // font: Windows tmAscent/tmDescent (OS/2 usWin* scaled), pixels
    uint32_t color;                   // brush
} Obj;
static Obj objs[NOBJ];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   // FreeType faces are not thread-safe; GDI is
static FT_Library ft;
static FT_Face faces[16];
static int nfaces;

static uint32_t new_obj(int type) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NOBJ; i++)
        if (objs[i].type == O_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].type = type;
            pthread_mutex_unlock(&lock);
            return BASE + 4 * i;
        }
    die("GDI handle table full");
}
static Obj *obj(uint32_t h, int type) {
    uint32_t i = (h - BASE) / 4;
    return h >= BASE && i < NOBJ && objs[i].type == type ? &objs[i] : NULL;
}
static Obj *dc_bmp(uint32_t dc) { Obj *d = obj(dc, O_DC); return d ? obj(d->bmp, O_BMP) : NULL; }
// font selected in a DC, sized and ready for FT_Load_Char; call with the lock held
static Obj *dc_font(uint32_t dc) {
    Obj *d = obj(dc, O_DC), *f = d ? obj(d->font, O_FONT) : NULL;
    if (f && !f->face) f = NULL;
    if (f) FT_Set_Char_Size(f->face, 0, f->em, 72, 72);
    return f;
}

IMP(G, AddFontMemResourceEx) {   // (pbFont, cbFont, pdv, &nFonts): FreeType reads the bytes in place, so keep a copy
    uint32_t n = ARG(c, 1);
    void *data = malloc(n);
    memcpy(data, GM(ARG(c, 0)), n);
    pthread_mutex_lock(&lock);
    if (!ft && FT_Init_FreeType(&ft)) die("FreeType init failed");
    FT_Face f;
    int ok = nfaces < 16 && !FT_New_Memory_Face(ft, data, n, 0, &f);
    if (ok) faces[nfaces++] = f;
    pthread_mutex_unlock(&lock);
    if (ok) fprintf(stderr, "[gdi32] font \"%s\" (%s)\n", f->family_name, f->style_name);
    U32(ARG(c, 3)) = ok;
    ret_std(c, ok ? 0xC0DF0000u + nfaces : 0, 4);
}

// CreateFontA(height, width, escapement, orientation, weight, italic, underline, strikeout, charset, out precision,
// clip precision, quality, pitch and family, face name). Only height and face name matter for the game's fonts.
IMP(G, CreateFontA) {
    int32_t height = (int32_t)ARG(c, 0);
    const char *name = ARG(c, 13) ? GSTR(ARG(c, 13)) : "";
    FT_Face face = NULL;
    for (int i = 0; i < nfaces && !face; i++)
        if (!strcasecmp(faces[i]->family_name, name)) face = faces[i];
    if (!face && nfaces) { fprintf(stderr, "[gdi32] no font \"%s\", using \"%s\"\n", name, faces[0]->family_name); face = faces[0]; }
    uint32_t h = new_obj(O_FONT);
    // Windows would substitute a system font; here a font made before AddFontMemResourceEx draws and measures
    // nothing. The game only does that from its device-reset handler, and its loader redraws the glyphs afterwards.
    if (!face) { fprintf(stderr, "[gdi32] CreateFontA(\"%s\") before any font was loaded: draws nothing\n", name); ret_std(c, h, 14); return; }
    TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    double up = face->units_per_EM;
    double asc = os2 ? os2->usWinAscent : face->ascender, desc = os2 ? os2->usWinDescent : -face->descender;
    // negative height = em size (character height), positive = cell height (ascent + descent), 0 = default
    double em = height < 0 ? -height : height > 0 ? height * up / (asc + desc) : 16;
    Obj *f = obj(h, O_FONT);
    f->face = face;
    f->em = (FT_F26Dot6)(em * 64 + 0.5);
    f->ascent = (int)(asc * em / up + 0.5);
    f->descent = (int)(desc * em / up + 0.5);
    ret_std(c, h, 14);
}

IMP(G, CreateCompatibleDC) { uint32_t h = new_obj(O_DC); obj(h, O_DC)->bmp = STOCK_BITMAP; obj(h, O_DC)->font = STOCK(13); ret_std(c, h, 1); }
IMP(G, CreateCompatibleBitmap) {   // (dc, w, h): screen-compatible = 32 bits per pixel, starts black
    int w = (int)ARG(c, 1), hh = (int)ARG(c, 2);
    if (w <= 0 || hh <= 0) { ret_std(c, 0, 3); return; }
    uint32_t h = new_obj(O_BMP);
    Obj *b = obj(h, O_BMP);
    b->w = w; b->h = hh;
    b->px = calloc((size_t)w * hh, 4);
    ret_std(c, h, 3);
}
IMP(G, CreateSolidBrush) { uint32_t h = new_obj(O_BRUSH); obj(h, O_BRUSH)->color = ARG(c, 0); ret_std(c, h, 1); }
IMP(G, GetStockObject) { ret_std(c, STOCK(ARG(c, 0) & 0xFF), 1); }

IMP(G, SelectObject) {   // (dc, obj): returns the previously selected object of the same kind
    Obj *d = obj(ARG(c, 0), O_DC);
    uint32_t h = ARG(c, 1), prev = 0;
    if (d) {
        if (obj(h, O_BMP) || h == STOCK_BITMAP) prev = d->bmp, d->bmp = h;
        else if (obj(h, O_FONT) || (h >= STOCK(10) && h <= STOCK(17))) prev = d->font, d->font = h;
        else prev = STOCK(0);   // brushes/pens: the game never draws with a selected one
    }
    ret_std(c, prev, 2);
}
IMP(G, DeleteObject) {   // DCs hold handles, not pointers, so a deleted object just stops being drawn to
    pthread_mutex_lock(&lock);
    Obj *o = obj(ARG(c, 0), O_BMP);
    if (o) free(o->px);
    if (!o) o = obj(ARG(c, 0), O_FONT);
    if (!o) o = obj(ARG(c, 0), O_BRUSH);
    if (o) o->type = O_FREE;
    pthread_mutex_unlock(&lock);
    ret_std(c, 1, 1);
}
IMP(G, DeleteDC) {
    pthread_mutex_lock(&lock);
    Obj *d = obj(ARG(c, 0), O_DC);
    if (d) d->type = O_FREE;
    pthread_mutex_unlock(&lock);
    ret_std(c, d != NULL, 1);
}
IMP(G, SetBkMode) { ret_std(c, 1, 2); }   // previous: TRANSPARENT; text is always drawn without a background box
IMP(G, SetTextColor) {
    Obj *d = obj(ARG(c, 0), O_DC);
    uint32_t prev = d ? d->text_color : 0xFFFFFFFFu;
    if (d) d->text_color = ARG(c, 1);
    ret_std(c, prev, 2);
}
IMP(G, SetMapMode) { ret_std(c, 1, 2); }   // previous: MM_TEXT. No extents are ever set, so every mode is 1:1 here

static uint32_t rgb(uint32_t colorref) { return (colorref & 0xFF) << 16 | (colorref & 0xFF00) | (colorref >> 16 & 0xFF); }

IMPX("USER32.dll", "FillRect", FillRect) {   // (dc, RECT*, brush)
    static const uint32_t stock[] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0};   // WHITE..BLACK_BRUSH
    Obj *b = dc_bmp(ARG(c, 0)), *br = obj(ARG(c, 2), O_BRUSH);
    uint32_t sh = ARG(c, 2), color;
    if (br) color = rgb(br->color);
    else if (sh >= STOCK(0) && sh <= STOCK(4)) color = stock[(sh - STOCK(0)) / 4];
    else { ret_std(c, 1, 3); return; }   // NULL_BRUSH, system colors: nothing to paint
    if (b) {
        int32_t *r = (int32_t *)GM(ARG(c, 1));
        int x0 = r[0] < 0 ? 0 : r[0], y0 = r[1] < 0 ? 0 : r[1];
        int x1 = r[2] > b->w ? b->w : r[2], y1 = r[3] > b->h ? b->h : r[3];
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) b->px[y * b->w + x] = color;
    }
    ret_std(c, 1, 3);
}

IMP(G, TextOutW) {   // (dc, x, y, str, n): top-left alignment, transparent background, antialiased
    Obj *d = obj(ARG(c, 0), O_DC), *b = dc_bmp(ARG(c, 0));
    int x = (int32_t)ARG(c, 1), y = (int32_t)ARG(c, 2), n = (int)ARG(c, 4);
    const uint16_t *s = (const uint16_t *)GM(ARG(c, 3));
    pthread_mutex_lock(&lock);
    Obj *f = dc_font(ARG(c, 0));
    if (d && b && f) {
        uint32_t col = rgb(d->text_color);
        FT_Pos pen = (FT_Pos)x * 64;
        static uint8_t seen[0x10000 / 8];   // CATH_TRACE_GDI=1: each character the font has no glyph for, once
        for (int i = 0; i < n; i++) {
            if (getenv("CATH_TRACE_GDI") && !FT_Get_Char_Index(f->face, s[i]) && !(seen[s[i] / 8] & 1 << s[i] % 8)) {
                seen[s[i] / 8] |= 1 << s[i] % 8;
                fprintf(stderr, "[gdi32] TextOutW U+%04X: no glyph in \"%s\"\n", s[i], f->face->family_name);
            }
            // Windows GDI draws nothing and moves the pen 0 for these when the font has no glyph (measured with the
            // game's fonts); FreeType would draw the .notdef box. Phone mails start paragraphs with a '\n' the game's
            // line splitter (0x56b5f0) leaves in, and its glyph cache (0x5d4350) draws it over the next letter.
            uint16_t ch = s[i];
            if ((ch == 9 || ch == 10 || ch == 13 || (ch >= 0x1c && ch <= 0x1f)) && !FT_Get_Char_Index(f->face, ch)) continue;
            if (FT_Load_Char(f->face, s[i], FT_LOAD_RENDER)) continue;
            FT_GlyphSlot g = f->face->glyph;
            int gx = (int)(pen >> 6) + g->bitmap_left, gy = y + f->ascent - g->bitmap_top;
            for (unsigned r = 0; r < g->bitmap.rows; r++)
                for (unsigned k = 0; k < g->bitmap.width; k++) {
                    int px = gx + (int)k, py = gy + (int)r;
                    unsigned a = g->bitmap.buffer[r * g->bitmap.pitch + k];
                    if (!a || px < 0 || py < 0 || px >= b->w || py >= b->h) continue;
                    uint32_t *p = &b->px[py * b->w + px], o = 0;
                    for (int sh = 0; sh < 24; sh += 8)
                        o |= ((((*p >> sh) & 0xFF) * (255 - a) + ((col >> sh) & 0xFF) * a + 127) / 255) << sh;
                    *p = o;
                }
            pen += g->advance.x;
        }
    }
    pthread_mutex_unlock(&lock);
    ret_std(c, d && b && f, 5);
}

// per-character widths in pixels: ABC = left bearing, ink width, right bearing (A + B + C = advance)
static int char_widths(uint32_t dc, uint32_t first, uint32_t last, uint32_t out, int abc) {
    pthread_mutex_lock(&lock);
    Obj *f = dc_font(dc);
    for (uint32_t ch = first; f && ch <= last; ch++) {
        float *o = (float *)GM(out + (ch - first) * (abc ? 12 : 4));
        if (FT_Load_Char(f->face, ch, FT_LOAD_DEFAULT)) { memset(o, 0, abc ? 12 : 4); continue; }
        FT_Glyph_Metrics *m = &f->face->glyph->metrics;
        float adv = f->face->glyph->advance.x / 64.f;
        if (!abc) { o[0] = adv; continue; }
        o[0] = m->horiBearingX / 64.f;
        o[1] = m->width / 64.f;
        o[2] = adv - o[0] - o[1];
    }
    pthread_mutex_unlock(&lock);
    return f != NULL;
}
IMP(G, GetCharABCWidthsFloatW) { ret_std(c, char_widths(ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3), 1), 4); }
IMP(G, GetCharWidthFloatW) { ret_std(c, char_widths(ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3), 0), 4); }

IMP(G, GetTextMetricsW) {   // (dc, TEXTMETRICW*): 11 LONGs, 4 WCHARs, 5 BYTEs (60 bytes with padding)
    pthread_mutex_lock(&lock);
    Obj *f = dc_font(ARG(c, 0));
    if (f) {
        uint8_t *t = GM(ARG(c, 1));
        int32_t *l = (int32_t *)t;
        TT_OS2 *os2 = FT_Get_Sfnt_Table(f->face, FT_SFNT_OS2);
        double px = f->em / 64.0 / f->face->units_per_EM;
        memset(t, 0, 60);
        l[0] = f->ascent + f->descent;                                         // tmHeight
        l[1] = f->ascent;
        l[2] = f->descent;
        l[3] = l[0] - (int)(f->em / 64.0 + 0.5);                               // tmInternalLeading
        l[5] = (int)((os2 ? os2->xAvgCharWidth : f->face->max_advance_width / 2) * px + 0.5);   // tmAveCharWidth
        l[6] = (int)(f->face->max_advance_width * px + 0.5);
        l[7] = os2 ? os2->usWeightClass : 400;
        l[9] = l[10] = 96;                                                     // tmDigitizedAspectX/Y
        uint16_t *w = (uint16_t *)(t + 44);
        w[0] = 0x20; w[1] = 0xFFFC; w[2] = 0x1F; w[3] = 0x20;                  // first, last, default, break char
        t[55] = 0x06;                                                          // TMPF_VECTOR | TMPF_TRUETYPE
        static uint32_t traced[64];   // CATH_TRACE_GDI=1: each font once (compare with real Windows GDI)
        uint32_t fh = obj(ARG(c, 0), O_DC)->font;
        if (getenv("CATH_TRACE_GDI") && fh >= BASE && !traced[(fh - BASE) / 4 % 64]) {
            traced[(fh - BASE) / 4 % 64] = 1;
            FT_Load_Char(f->face, 'A', FT_LOAD_DEFAULT);
            fprintf(stderr, "[gdi32] metrics \"%s\" em %.1f: tmHeight %d ascent %d descent %d intLead %d ave %d max %d weight %d; 'A' bearing %.1f width %.1f advance %.1f\n",
                    f->face->family_name, f->em / 64.0, l[0], l[1], l[2], l[3], l[5], l[6], l[7], f->face->glyph->metrics.horiBearingX / 64.0,
                    f->face->glyph->metrics.width / 64.0, f->face->glyph->advance.x / 64.0);
        }
    }
    pthread_mutex_unlock(&lock);
    ret_std(c, f != NULL, 2);
}

// GetDIBits(dc, bitmap, first line, lines, bits, BITMAPINFO*, usage): only the 32-bit BI_RGB copy the game asks for
IMP(G, GetDIBits) {
    Obj *b = obj(ARG(c, 1), O_BMP);
    int32_t *bi = (int32_t *)GM(ARG(c, 5));   // biSize, biWidth, biHeight, (biPlanes | biBitCount << 16), biCompression
    uint32_t start = ARG(c, 2), lines = ARG(c, 3), out = ARG(c, 4);
    if (!b || !out || (bi[3] >> 16) != 32 || bi[4] != 0 || bi[1] != b->w)
        die("GetDIBits: unsupported request (bitmap %s, bits %08x, %dx%d, %d bpp, compression %d)",
            b ? "ok" : "unknown", out, bi[1], bi[2], bi[3] >> 16, bi[4]);
    if (start >= (uint32_t)b->h) lines = 0;
    else if (lines > b->h - start) lines = b->h - start;
    for (uint32_t i = 0; i < lines; i++) {   // negative biHeight = top-down rows, positive = bottom-up
        uint32_t row = bi[2] < 0 ? start + i : b->h - 1 - (start + i);
        memcpy(GM(out + i * b->w * 4), &b->px[row * b->w], b->w * 4);
    }
    static int dumps;
    if (getenv("CATH_DUMP_FONTS") && lines) {   // debug: glyph sheet as a grayscale image (blue = what the game keeps)
        char name[64];
        snprintf(name, sizeof name, "/tmp/cath_font_%d.pgm", dumps++);
        FILE *fp = fopen(name, "wb");
        fprintf(fp, "P5 %d %d 255\n", b->w, b->h);
        for (int i = 0; i < b->w * b->h; i++) fputc(b->px[i] & 0xFF, fp);
        fclose(fp);
    }
    ret_std(c, lines, 7);
}
