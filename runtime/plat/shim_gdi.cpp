/* shim_gdi.cpp - the GDI half of the Win32 compatibility layer (see windows.h).
 *
 * A DC draws into an ARGB8888 software canvas (a window's backing store, or the bitmap of a
 * memory DC). Shapes are plain GDI-style (no antialiasing, like Windows), text is rasterized
 * with stb_truetype from the system fonts (Segoe UI -> Liberation Sans / DejaVu Sans, MS Gothic
 * -> the CJK font of the system, with a fallback chain so every code point has a glyph).
 * StretchDIBits/SetDIBitsToDevice take the Windows 32-bpp BI_RGB layout (BGRX in memory) and
 * convert to the canvas layout; the game's own bilinear scaler (gfx.cpp) already produced the
 * exact pixels, so nearest-neighbour is all the blit needs.
 *
 * Every GDI object is one struct (tf_hgdi_s) told apart by its kind, exactly like the real
 * handles are interchangeable HGDIOBJs.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "windows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

typedef std::string S; typedef std::wstring W;

/* ------------------------------------------------------------------ types */
struct Canvas {
    uint32_t *px = nullptr;
    int w = 0, h = 0;
    bool owned = true;        /* window canvases are freed by the window */
};
struct Face;                 /* a loaded TTF */
struct tf_hdc_s {
    Canvas *cv = nullptr;
    int orgx = 0, orgy = 0;   /* window-local origin (child DCs offset into the parent canvas) */
    RECT clip{0, 0, 0, 0};
    HGDIOBJ brush = nullptr, pen = nullptr, font = nullptr, bmp = nullptr;
    COLORREF text = 0, bk = 0;
    int bkmode = OPAQUE;
    POINT cur{0, 0};
    bool *touched = nullptr;  /* the window's present flag (null for memory DCs) */
    int rop2 = R2_COPYPEN;
};
struct tf_hgdi_s {
    int kind = 0;                                          /* 1 brush, 2 pen, 3 font, 4 bitmap, 5 stock */
    COLORREF color = 0;                                    /* brush / pen */
    int style = 0, width = 1;                              /* pen */
    float px = 16.0f; int weight = 400;                    /* font: em size in pixels, weight */
    std::vector<Face *> chain;                             /* font faces, primary first */
    Canvas cv{nullptr, 0, 0, true};                        /* bitmap */
};

static tf_hdc_s *DC(HDC h) { return (tf_hdc_s *)h; }
static tf_hgdi_s *GO(HGDIOBJ h) { return (tf_hgdi_s *)h; }
static inline void touch(tf_hdc_s *dc) { if (dc->touched) *dc->touched = true; }

/* ------------------------------------------------------------------ canvas + DC */
Canvas *tf_gdi_new_canvas(int w, int h) {
    Canvas *c = new Canvas();
    c->w = std::max(1, w); c->h = std::max(1, h);
    c->px = (uint32_t *)calloc((size_t)c->w * c->h, 4);
    return c;
}
void tf_gdi_free_canvas(Canvas *c) { if (!c) return; if (c->owned && c->px) free(c->px); delete c; }
void tf_gdi_canvas_pixels(Canvas *c, uint32_t **px, int *stride, int *) { if (px) *px = c->px; if (stride) *stride = c->w * 4; }

HDC tf_gdi_dc_for_canvas(Canvas *c, int orgx, int orgy) {
    if (!c) return nullptr;
    tf_hdc_s *dc = new tf_hdc_s();
    dc->cv = c; dc->orgx = orgx; dc->orgy = orgy;
    dc->clip = {0, 0, c->w, c->h};
    return (HDC)dc;
}
void tf_gdi_dc_set_touched(HDC h, bool *touched) { if (h) DC(h)->touched = touched; }   /* window DCs: a GDI draw marks the window for present */
void tf_gdi_free_dc(HDC h) { delete DC(h); }

/* ------------------------------------------------------------------ fonts (stb_truetype) */
struct Face {
    std::string path;
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool ok = false;
    int ascent = 0, descent = 0, linegap = 0;   /* font units */
};
static std::vector<std::string> g_font_dirs;
static std::map<std::string, Face *> g_faces;   /* by path */
static std::mutex g_fmu;

static void font_dirs_init(void) {
    if (!g_font_dirs.empty()) return;
    const char *dirs[] = {
        "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/truetype/dejavu",
        "/usr/share/fonts/truetype/japanese", "/usr/share/fonts/truetype/chinese",
        "/usr/share/fonts/opentype/noto", "/usr/share/fonts/truetype/noto", "/usr/share/fonts/truetype/noto-serif-sc",
        "/usr/share/fonts/truetype/freefont", "/usr/share/fonts/truetype/libreoffice", "/usr/share/fonts/truetype/wqy",
        "/usr/share/fonts",
    };
    for (auto *d : dirs) g_font_dirs.push_back(d);
    if (const char *e = getenv("TF_FONT_DIR")) g_font_dirs.insert(g_font_dirs.begin(), e);
}
static bool file_readable(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static Face *load_face(const std::string &path) {
    std::lock_guard<std::mutex> lk(g_fmu);
    auto it = g_faces.find(path);
    if (it != g_faces.end()) return it->second;
    Face *f = new Face();
    f->path = path;
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n > 0 && n < 60 * 1024 * 1024) {
            f->data.resize((size_t)n);
            if (fread(f->data.data(), 1, (size_t)n, fp) == (size_t)n)
                f->ok = stbtt_InitFont(&f->info, f->data.data(), 0) != 0;
        }
        fclose(fp);
    }
    if (f->ok) {
        int a, d, g2;
        stbtt_GetFontVMetrics(&f->info, &a, &d, &g2);
        f->ascent = a; f->descent = d; f->linegap = g2;
    }
    g_faces[path] = f;
    return f;
}
static Face *find_face(const char *basename_lower) {
    font_dirs_init();
    std::string want = basename_lower;
    for (auto &c : want) c = (char)tolower((unsigned char)c);
    for (auto &d : g_font_dirs) {
        DIR *dir = opendir(d.c_str());
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir))) {
            std::string n = e->d_name;
            std::string ln = n;
            for (auto &c : ln) c = (char)tolower((unsigned char)c);
            if (ln.find(want) != std::string::npos && ln.size() > 4 && ln.substr(ln.size() - 4) == ".ttf") {
                std::string full = d + "/" + n;
                if (file_readable(full.c_str())) { closedir(dir); return load_face(full); }
            }
        }
        closedir(dir);
    }
    return nullptr;
}
/*the first face with the glyph wins (Latin first, CJK behind)*/
static Face *first_face_with(tf_hgdi_s *fo, int codepoint, int *glyph) {
    for (Face *fc : fo->chain) {
        if (!fc || !fc->ok) continue;
        int g = stbtt_FindGlyphIndex(&fc->info, codepoint);
        if (g) { if (glyph) *glyph = g; return fc; }
    }
    if (glyph) *glyph = 0;
    return (!fo->chain.empty() && fo->chain[0] && fo->chain[0]->ok) ? fo->chain[0] : nullptr;
}
static Face *face_liberation(bool bold) {
    Face *f = find_face(bold ? "liberationsans-bold" : "liberationsans-regular");
    if (!f || !f->ok) f = find_face(bold ? "dejavusans-bold" : "dejavusans");
    return (f && f->ok) ? f : nullptr;
}
static Face *face_cjk(void) {
    Face *f = find_face("fonts-japanese-gothic");
    if (!f || !f->ok) f = find_face("notosanssc-regular");
    if (!f || !f->ok) f = find_face("notosanscjk");
    if (!f || !f->ok) f = find_face("wenquanyi");
    return (f && f->ok) ? f : nullptr;
}
HFONT CreateFontW(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR face) {
    tf_hgdi_s *f = new tf_hgdi_s();
    f->kind = 3;
    f->px = height < 0 ? (float)-height : (float)(height ? height : 16);
    f->weight = weight ? weight : 400;
    W w = face ? face : L"";
    S n;
    for (wchar_t c : w) n += (char)(c < 128 ? tolower((int)c) : '?');
    bool cjk_name = n.find("gothic") != S::npos || n.find("mincho") != S::npos || n.find("cjk") != S::npos ||
                    n.find("noto sans sc") != S::npos || n.find("yu gothic") != S::npos || n.find("meiryo") != S::npos;
    if (cjk_name) {
        if (Face *c = face_cjk()) f->chain.push_back(c);
        if (Face *l = face_liberation(f->weight >= 600)) f->chain.push_back(l);
    } else {
        bool bold = f->weight >= 600 || n.find("semibold") != S::npos || n.find("bold") != S::npos;
        if (Face *l = face_liberation(bold)) f->chain.push_back(l);
        if (Face *c = face_cjk()) f->chain.push_back(c);   /* every font falls back to CJK glyphs */
    }
    return f;
}

/* ------------------------------------------------------------------ objects */
static std::vector<tf_hgdi_s *> g_stocks;
HGDIOBJ GetStockObject(int i) {
    auto mk = [&](int kind, COLORREF c, int style, int width) -> tf_hgdi_s * {
        tf_hgdi_s *o = new tf_hgdi_s();
        o->kind = kind; o->color = c; o->style = style; o->width = width;
        g_stocks.push_back(o);
        return o;
    };
    if (i == DEFAULT_GUI_FONT || i == SYSTEM_FONT || i == ANSI_VAR_FONT) {
        static HFONT gui_font;
        if (!gui_font) gui_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        return gui_font;
    }
    static tf_hgdi_s *br[6] = {}, *pens[3] = {};
    if (i <= NULL_BRUSH) {
        if (!br[i]) br[i] = mk(1, i == WHITE_BRUSH ? 0xFFFFFF : i == BLACK_BRUSH ? 0x000000 : i == LTGRAY_BRUSH ? 0xBFBFBF : i == GRAY_BRUSH ? 0x808080 : i == DKGRAY_BRUSH ? 0x404040 : 0, 0, 1);
        return br[i];
    }
    if (i >= WHITE_PEN && i <= NULL_PEN) {
        int k = i - WHITE_PEN;
        if (!pens[k]) pens[k] = mk(2, i == WHITE_PEN ? 0xFFFFFF : 0, i == NULL_PEN ? PS_NULL : PS_SOLID, 1);
        return pens[k];
    }
    return nullptr;
}
HBRUSH CreateSolidBrush(COLORREF c) { tf_hgdi_s *b = new tf_hgdi_s(); b->kind = 1; b->color = c; return b; }
HPEN CreatePen(int style, int width, COLORREF c) { tf_hgdi_s *p = new tf_hgdi_s(); p->kind = 2; p->style = style; p->width = std::max(1, width); p->color = c; return p; }
HGDIOBJ SelectObject(HDC h, HGDIOBJ o) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !o) return nullptr;
    HGDIOBJ old = nullptr;
    switch (GO(o)->kind) {
    case 1: old = dc->brush; dc->brush = o; break;
    case 2: old = dc->pen; dc->pen = o; break;
    case 3: old = dc->font; dc->font = o; break;
    case 4: old = dc->bmp; dc->bmp = o; dc->cv = &GO(o)->cv; dc->clip = {0, 0, dc->cv->w, dc->cv->h}; dc->orgx = dc->orgy = 0; break;
    default: break;
    }
    return old;
}
BOOL DeleteObject(HGDIOBJ o) {
    if (!o) return FALSE;
    for (auto *s : g_stocks) if (s == o) return TRUE;   /* stock objects live forever */
    if (GO(o)->kind == 4 && GO(o)->cv.px) free(GO(o)->cv.px);
    delete GO(o);
    return TRUE;
}
HDC CreateCompatibleDC(HDC src) {
    tf_hdc_s *dc = new tf_hdc_s();
    static Canvas tiny{nullptr, 1, 1, true};
    dc->cv = &tiny;
    dc->clip = {0, 0, 1, 1};
    if (src) { dc->brush = DC(src)->brush; dc->pen = DC(src)->pen; dc->font = DC(src)->font; dc->text = DC(src)->text; dc->bk = DC(src)->bk; dc->bkmode = DC(src)->bkmode; }
    return dc;
}
BOOL DeleteDC(HDC h) { tf_gdi_free_dc(h); return TRUE; }
HBITMAP CreateCompatibleBitmap(HDC, int w, int h) {
    tf_hgdi_s *b = new tf_hgdi_s();
    b->kind = 4;
    b->cv.w = std::max(1, w); b->cv.h = std::max(1, h);
    b->cv.px = (uint32_t *)calloc((size_t)b->cv.w * b->cv.h, 4);
    return b;
}

/* ------------------------------------------------------------------ colors */
int SetTextColor(HDC h, COLORREF c) { tf_hdc_s *dc = DC(h); if (!dc) return 0; int o = (int)dc->text; dc->text = c; return o; }
COLORREF GetTextColor(HDC h) { return DC(h) ? DC(h)->text : 0; }
int SetBkColor(HDC h, COLORREF c) { tf_hdc_s *dc = DC(h); if (!dc) return 0; int o = (int)dc->bk; dc->bk = c; return o; }
COLORREF GetBkColor(HDC h) { return DC(h) ? DC(h)->bk : 0; }
int SetBkMode(HDC h, int m) { tf_hdc_s *dc = DC(h); if (!dc) return 0; int o = dc->bkmode; dc->bkmode = m; return o; }
int SetROP2(HDC h, int r) { tf_hdc_s *dc = DC(h); if (!dc) return 0; int o = dc->rop2; dc->rop2 = r; return o; }

/* ------------------------------------------------------------------ rasterizer */
static inline uint32_t cpack(COLORREF c) { return 0xFF000000u | (c & 0xFFFFFF); }
static void clip_rect(tf_hdc_s *dc, RECT *r) {
    r->left = std::max(r->left, dc->clip.left); r->top = std::max(r->top, dc->clip.top);
    r->right = std::min(r->right, dc->clip.right); r->bottom = std::min(r->bottom, dc->clip.bottom);
}
int FillRect(HDC h, const RECT *rr, HBRUSH b) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !rr || !b) return 0;
    RECT r = *rr;
    r.left += dc->orgx; r.top += dc->orgy; r.right += dc->orgx; r.bottom += dc->orgy;
    clip_rect(dc, &r);
    uint32_t c = cpack(GO(b)->color);
    for (int y = r.top; y < r.bottom; y++) {
        uint32_t *row = dc->cv->px + (size_t)y * dc->cv->w;
        for (int x = r.left; x < r.right; x++) row[x] = c;
    }
    touch(dc);
    return 1;
}
int FrameRect(HDC h, const RECT *rr, HBRUSH b) {
    RECT t{rr->left, rr->top, rr->right, rr->top + 1}, bo{rr->left, rr->bottom - 1, rr->right, rr->bottom};
    RECT l{rr->left, rr->top, rr->left + 1, rr->bottom}, r2{rr->right - 1, rr->top, rr->right, rr->bottom};
    FillRect(h, &t, b); FillRect(h, &bo, b); FillRect(h, &l, b); FillRect(h, &r2, b);
    return 1;
}
static void hline(tf_hdc_s *dc, int x0, int x1, int y, uint32_t c) {
    if (y < dc->clip.top || y >= dc->clip.bottom) return;
    if (x0 < dc->clip.left) x0 = dc->clip.left;
    if (x1 > dc->clip.right) x1 = dc->clip.right;
    uint32_t *row = dc->cv->px + (size_t)y * dc->cv->w;
    for (int x = x0; x < x1; x++) row[x] = c;
}
static void pen_line(tf_hdc_s *dc, int x0, int y0, int x1, int y1, const tf_hgdi_s *p) {
    if (!p || p->style == PS_NULL) return;
    uint32_t c = cpack(p->color);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int w = std::max(1, p->width);
    for (;;) {
        if (w <= 1) {
            if (x0 >= dc->clip.left && x0 < dc->clip.right && y0 >= dc->clip.top && y0 < dc->clip.bottom)
                dc->cv->px[(size_t)y0 * dc->cv->w + x0] = c;
        } else {
            for (int y = y0 - w / 2; y < y0 + (w + 1) / 2; y++)
                if (y >= dc->clip.top && y < dc->clip.bottom)
                    for (int x = x0 - w / 2; x < x0 + (w + 1) / 2; x++)
                        if (x >= dc->clip.left && x < dc->clip.right)
                            dc->cv->px[(size_t)y * dc->cv->w + x] = c;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
BOOL MoveToEx(HDC h, int x, int y, LPPOINT old) {
    tf_hdc_s *dc = DC(h); if (!dc) return FALSE;
    if (old) { old->x = dc->cur.x; old->y = dc->cur.y; }
    dc->cur = {x, y};
    return TRUE;
}
BOOL LineTo(HDC h, int x, int y) {
    tf_hdc_s *dc = DC(h); if (!dc) return FALSE;
    HGDIOBJ p = dc->pen ? dc->pen : GetStockObject(BLACK_PEN);
    pen_line(dc, dc->cur.x + dc->orgx, dc->cur.y + dc->orgy, x + dc->orgx, y + dc->orgy, GO(p));
    dc->cur = {x, y};
    touch(dc);
    return TRUE;
}
BOOL Polyline(HDC h, const POINT *pts, int n) {
    tf_hdc_s *dc = DC(h); if (!dc || !pts || n < 2) return FALSE;
    HGDIOBJ p = dc->pen ? dc->pen : GetStockObject(BLACK_PEN);
    for (int i = 1; i < n; i++) pen_line(dc, pts[i - 1].x + dc->orgx, pts[i - 1].y + dc->orgy, pts[i].x + dc->orgx, pts[i].y + dc->orgy, GO(p));
    touch(dc);
    return TRUE;
}
BOOL Rectangle(HDC h, int x1, int y1, int x2, int y2) {
    tf_hdc_s *dc = DC(h); if (!dc) return FALSE;
    x1 += dc->orgx; y1 += dc->orgy; x2 += dc->orgx; y2 += dc->orgy;
    if (dc->brush) { RECT r{x1, y1, x2, y2}; FillRect(h, &r, (HBRUSH)dc->brush); }
    HGDIOBJ p = dc->pen ? dc->pen : GetStockObject(BLACK_PEN);
    if (GO(p)->style != PS_NULL) {
        pen_line(dc, x1, y1, x2 - 1, y1, GO(p)); pen_line(dc, x1, y2 - 1, x2 - 1, y2 - 1, GO(p));
        pen_line(dc, x1, y1, x1, y2 - 1, GO(p)); pen_line(dc, x2 - 1, y1, x2 - 1, y2 - 1, GO(p));
    }
    touch(dc);
    return TRUE;
}
static void fill_ellipse(tf_hdc_s *dc, int cx, int cy, int rx, int ry, uint32_t c) {
    int y0 = std::max<int>(dc->clip.top, cy - ry), y1 = std::min<int>(dc->clip.bottom, cy + ry);
    for (int y = y0; y < y1; y++) {
        double dy = (double)(y - cy) / ry;
        double span = 1.0 - dy * dy;
        if (span <= 0) continue;
        int half = (int)(rx * sqrt(span));
        hline(dc, cx - half, cx + half, y, c);
    }
}
BOOL Ellipse(HDC h, int x1, int y1, int x2, int y2) {
    tf_hdc_s *dc = DC(h); if (!dc) return FALSE;
    uint32_t c = dc->brush ? cpack(GO(dc->brush)->color) : cpack(dc->bk);
    fill_ellipse(dc, (x1 + x2) / 2 + dc->orgx, (y1 + y2) / 2 + dc->orgy, std::max(1, (x2 - x1) / 2), std::max(1, (y2 - y1) / 2), c);
    touch(dc);
    return TRUE;
}
BOOL RoundRect(HDC h, int x1, int y1, int x2, int y2, int ew, int eh) {
    tf_hdc_s *dc = DC(h); if (!dc) return FALSE;
    x1 += dc->orgx; y1 += dc->orgy; x2 += dc->orgx; y2 += dc->orgy;
    uint32_t fillc = dc->brush ? cpack(GO(dc->brush)->color) : cpack(dc->bk);
    HGDIOBJ p = dc->pen ? dc->pen : nullptr;
    uint32_t edgec = (p && GO(p)->style != PS_NULL) ? cpack(GO(p)->color) : fillc;
    int r = std::min<int>({ew, eh, (x2 - x1) / 2, (y2 - y1) / 2});
    if (r < 1) r = 1;
    const int ir2 = r * r;
    int ry0 = std::max<int>(dc->clip.top, y1), ry1 = std::min<int>(dc->clip.bottom, y2);
    for (int y = ry0; y < ry1; y++) {
        int cx1 = x1, cx2 = x2;
        if (y < y1 + r) { int dy = (y1 + r - 1) - y; int off = r - (int)sqrt((double)(ir2 - dy * dy)) - 1; cx1 = x1 + std::max(0, off); }
        if (y >= y2 - r) { int dy = y - (y2 - r); int off = r - (int)sqrt((double)(ir2 - dy * dy)) - 1; cx2 = x2 - std::max(0, off); }
        hline(dc, cx1, cx2, y, fillc);
        if (edgec != fillc) {
            if (cx1 >= dc->clip.left && cx1 < dc->clip.right) dc->cv->px[(size_t)y * dc->cv->w + cx1] = edgec;
            if (cx2 - 1 >= dc->clip.left && cx2 - 1 < dc->clip.right) dc->cv->px[(size_t)y * dc->cv->w + cx2 - 1] = edgec;
        }
    }
    if (edgec != fillc) { hline(dc, x1, x2, y1, edgec); hline(dc, x1, x2, y2 - 1, edgec); }
    touch(dc);
    return TRUE;
}
BOOL Polygon(HDC h, const POINT *pts, int n) {
    tf_hdc_s *dc = DC(h); if (!dc || !pts || n < 3) return FALSE;
    uint32_t c = dc->brush ? cpack(GO(dc->brush)->color) : cpack(dc->bk);
    int miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) { miny = std::min<int>(miny, pts[i].y); maxy = std::max<int>(maxy, pts[i].y); }
    std::vector<int> xs(n);
    int yy0 = std::max<int>(miny + dc->orgy, dc->clip.top), yy1 = std::min<int>(maxy + dc->orgy, dc->clip.bottom - 1);
    for (int y = yy0; y <= yy1; y++) {
        int k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            int yi = pts[i].y + dc->orgy, yj = pts[j].y + dc->orgy;
            if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
                int xi = pts[i].x + dc->orgx, xj = pts[j].x + dc->orgx;
                xs[k++] = xi + (y - yi) * (xj - xi) / (yj - yi);
            }
        }
        std::sort(xs.begin(), xs.begin() + k);
        for (int i = 0; i + 1 < k; i += 2) hline(dc, xs[i], xs[i + 1] + 1, y, c);
    }
    touch(dc);
    return TRUE;
}
COLORREF SetPixel(HDC h, int x, int y, COLORREF c) {
    tf_hdc_s *dc = DC(h); if (!dc) return 0;
    x += dc->orgx; y += dc->orgy;
    if (x < dc->clip.left || x >= dc->clip.right || y < dc->clip.top || y >= dc->clip.bottom) return 0;
    uint32_t *p = &dc->cv->px[(size_t)y * dc->cv->w + x];
    uint32_t o = *p;
    *p = cpack(c);
    touch(dc);
    return o & 0xFFFFFF;
}
COLORREF GetPixel(HDC h, int x, int y) {
    tf_hdc_s *dc = DC(h); if (!dc) return 0;
    x += dc->orgx; y += dc->orgy;
    if (x < 0 || x >= dc->cv->w || y < 0 || y >= dc->cv->h) return 0;
    return dc->cv->px[(size_t)y * dc->cv->w + x] & 0xFFFFFF;
}
BOOL PatBlt(HDC, int, int, int, int, DWORD) { return TRUE; }
int SaveDC(HDC) { return 1; }
BOOL RestoreDC(HDC, int) { return TRUE; }
void GdiFlush(void) {}
int SetStretchBltMode(HDC, int) { return 1; }
int GetDeviceCaps(HDC, int m) { if (m == LOGPIXELSX || m == LOGPIXELSY) return 96; if (m == BITSPIXEL) return 32; if (m == PLANES) return 1; if (m == HORZRES || m == VERTRES) return 1920; return 0; }

/* ------------------------------------------------------------------ blits (Windows BI_RGB 32bpp: B,G,R,X in memory -> canvas ARGB) */
static uint32_t bgrx_to_argb(uint32_t p) { return 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF); }
int StretchDIBits(HDC h, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const void *bits, const BITMAPINFO *bi, UINT, DWORD rop) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !bi || !bits || rop != SRCCOPY) return 0;
    const BITMAPINFOHEADER &b = bi->bmiHeader;
    int srcw = b.biWidth, srch = abs(b.biHeight);
    bool flip = b.biHeight > 0;   /* positive height = bottom-up rows */
    dx += dc->orgx; dy += dc->orgy;
    for (int y = 0; y < dh; y++) {
        int syy = sy + y * sh / dh;
        if (syy < 0 || syy >= srch) continue;
        int dyy = dy + y;
        if (dyy < dc->clip.top || dyy >= dc->clip.bottom) continue;
        const uint32_t *srow = (const uint32_t *)bits + (size_t)(flip ? srch - 1 - syy : syy) * srcw;
        uint32_t *drow = dc->cv->px + (size_t)dyy * dc->cv->w;
        for (int x = 0; x < dw; x++) {
            int sxx = sx + x * sw / dw;
            if (sxx < 0 || sxx >= srcw) continue;
            int dxx = dx + x;
            if (dxx < dc->clip.left || dxx >= dc->clip.right) continue;
            drow[dxx] = bgrx_to_argb(srow[sxx]);
        }
    }
    touch(dc);
    return dh;
}
int SetDIBitsToDevice(HDC h, int dx, int dy, int dw, int dh, int, int, UINT, UINT, const void *bits, const BITMAPINFO *bi, UINT) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !bi || !bits) return 0;
    const BITMAPINFOHEADER &b = bi->bmiHeader;
    int srcw = b.biWidth, srch = abs(b.biHeight);
    bool flip = b.biHeight > 0;
    dx += dc->orgx; dy += dc->orgy;
    for (int y = 0; y < dh; y++) {
        if (y >= srch) break;
        int dyy = dy + y;
        if (dyy < dc->clip.top || dyy >= dc->clip.bottom) continue;
        const uint32_t *srow = (const uint32_t *)bits + (size_t)(flip ? srch - 1 - y : y) * srcw;
        uint32_t *drow = dc->cv->px + (size_t)dyy * dc->cv->w;
        for (int x = 0; x < dw; x++) {
            if (x >= srcw) break;
            int dxx = dx + x;
            if (dxx < dc->clip.left || dxx >= dc->clip.right) continue;
            drow[dxx] = bgrx_to_argb(srow[x]);
        }
    }
    touch(dc);
    return dh;
}
BOOL BitBlt(HDC hd, int dx, int dy, int w, int h, HDC hs, int sx, int sy, DWORD rop) {
    tf_hdc_s *d = DC(hd), *s = DC(hs);
    if (!d || !s || rop != SRCCOPY) return FALSE;
    dx += d->orgx; dy += d->orgy; sx += s->orgx; sy += s->orgy;
    for (int y = 0; y < h; y++) {
        int dyy = dy + y, syy = sy + y;
        if (dyy < d->clip.top || dyy >= d->clip.bottom || syy < 0 || syy >= s->cv->h) continue;
        uint32_t *drow = d->cv->px + (size_t)dyy * d->cv->w;
        const uint32_t *srow = s->cv->px + (size_t)syy * s->cv->w;
        for (int x = 0; x < w; x++) {
            int dxx = dx + x, sxx = sx + x;
            if (dxx < d->clip.left || dxx >= d->clip.right || sxx < 0 || sxx >= s->cv->w) continue;
            drow[dxx] = srow[sxx];
        }
    }
    touch(d);
    return TRUE;
}

/* ------------------------------------------------------------------ text */
static tf_hgdi_s *cur_font(tf_hdc_s *dc) {
    if (dc->font) return GO(dc->font);
    return GO(GetStockObject(DEFAULT_GUI_FONT));
}
struct GM { int adv, x0, y0, x1, y1; Face *f; int glyph; };
static float font_scale(Face *f, float px) { return stbtt_ScaleForPixelHeight(&f->info, px); }
static GM glyph_metrics(tf_hgdi_s *fo, int code) {
    GM g{0, 0, 0, 0, 0, nullptr, 0};
    Face *f = first_face_with(fo, code, &g.glyph);
    if (!f) return g;
    float sc = font_scale(f, fo->px);
    int adv = 0, lsb = 0;
    if (g.glyph) stbtt_GetGlyphHMetrics(&f->info, g.glyph, &adv, &lsb);
    else stbtt_GetCodepointHMetrics(&f->info, code, &adv, &lsb);
    g.adv = (int)(adv * sc + 0.5f);
    if (g.glyph) stbtt_GetGlyphBitmapBox(&f->info, g.glyph, sc, sc, &g.x0, &g.y0, &g.x1, &g.y1);
    else stbtt_GetCodepointBitmapBox(&f->info, code, sc, sc, &g.x0, &g.y0, &g.x1, &g.y1);
    g.f = f;
    return g;
}
BOOL GetTextMetricsW(HDC h, LPTEXTMETRICW tm) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !tm) return FALSE;
    tf_hgdi_s *fo = cur_font(dc);
    memset(tm, 0, sizeof *tm);
    Face *f = first_face_with(fo, 'n', nullptr);
    if (!f) return TRUE;
    float sc = font_scale(f, fo->px);
    int asc = (int)(f->ascent * sc + 0.5f), desc = -(int)(f->descent * sc + 0.5f);
    tm->tmAscent = asc; tm->tmDescent = desc; tm->tmHeight = asc + desc;
    tm->tmInternalLeading = std::max<int>(0, tm->tmHeight - (int)fo->px);
    GM g = glyph_metrics(fo, 'n');
    tm->tmAveCharWidth = std::max(1, g.adv ? g.adv : (int)fo->px / 2);
    tm->tmMaxCharWidth = (int)fo->px;
    tm->tmWeight = fo->weight;
    tm->tmPitchAndFamily = 4;   /* variable pitch */
    tm->tmCharSet = DEFAULT_CHARSET;
    return TRUE;
}
BOOL GetTextExtentPoint32W(HDC h, LPCWSTR s, int n, LPSIZE sz) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !sz) return FALSE;
    if (n < 0) n = (int)wcslen(s ? s : L"");
    if (!s) { sz->cx = 0; sz->cy = 0; return TRUE; }
    tf_hgdi_s *fo = cur_font(dc);
    Face *f = first_face_with(fo, 'n', nullptr);
    int asc = (int)fo->px, desc = 4;
    if (f) { float sc = font_scale(f, fo->px); asc = (int)(f->ascent * sc + 0.5f); desc = -(int)(f->descent * sc + 0.5f); }
    int x = 0;
    for (int i = 0; i < n; i++) x += glyph_metrics(fo, s[i]).adv;
    sz->cx = x; sz->cy = asc + desc;
    return TRUE;
}
static void draw_glyph(tf_hdc_s *dc, tf_hgdi_s *fo, int code, int px, int py, COLORREF col) {
    GM g = glyph_metrics(fo, code);
    if (!g.f) return;
    float sc = font_scale(g.f, fo->px);
    int w = g.x1 - g.x0, h = g.y1 - g.y0;
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> cov((size_t)w * h);
    if (g.glyph) stbtt_MakeGlyphBitmap(&g.f->info, cov.data(), w, h, w, sc, sc, g.glyph);
    else stbtt_MakeCodepointBitmap(&g.f->info, cov.data(), w, h, w, sc, sc, code);
    int dx = px + g.x0 + dc->orgx, dy = py + g.y0 + dc->orgy;
    uint8_t cr = (uint8_t)(col & 0xFF), cg = (uint8_t)((col >> 8) & 0xFF), cb = (uint8_t)((col >> 16) & 0xFF);
    for (int y = 0; y < h; y++) {
        int dyy = dy + y;
        if (dyy < dc->clip.top || dyy >= dc->clip.bottom) continue;
        uint32_t *row = dc->cv->px + (size_t)dyy * dc->cv->w;
        for (int x = 0; x < w; x++) {
            int dxx = dx + x;
            if (dxx < dc->clip.left || dxx >= dc->clip.right) continue;
            uint32_t a = cov[(size_t)y * w + x];
            if (!a) continue;
            uint32_t d = row[dxx];
            uint32_t da = 255 - a;
            uint8_t dr = (uint8_t)(d & 0xFF), dg = (uint8_t)((d >> 8) & 0xFF), db = (uint8_t)((d >> 16) & 0xFF);
            row[dxx] = 0xFF000000u | ((uint32_t)(cb * a + db * da) / 255) | ((uint32_t)(cg * a + dg * da) / 255) << 8 | ((uint32_t)(cr * a + dr * da) / 255) << 16;
        }
    }
}
BOOL TextOutW(HDC h, int x, int y, LPCWSTR s, int n) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !s) return FALSE;
    if (n < 0) n = (int)wcslen(s);
    tf_hgdi_s *fo = cur_font(dc);
    Face *f = first_face_with(fo, 'n', nullptr);
    int asc = (int)fo->px;
    if (f) asc = (int)(f->ascent * font_scale(f, fo->px) + 0.5f);
    int px = x, py = y + asc;   /* draw_glyph adds the DC origin itself */
    for (int i = 0; i < n; i++) { draw_glyph(dc, fo, s[i], px, py, dc->text); px += glyph_metrics(fo, s[i]).adv; }
    touch(dc);
    return TRUE;
}
BOOL ExtTextOutW(HDC h, int x, int y, UINT, const RECT *, LPCWSTR s, UINT n, const int *) { return TextOutW(h, x, y, s, (int)n); }

/*DrawTextW: the flags the runtime uses (CALCRECT, WORDBREAK, CENTER/VCENTER, ellipsis...)*/
int DrawTextW(HDC h, LPCWSTR s, int n, LPRECT r, UINT flags) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !r) return 0;
    if (!s) s = L"";
    if (n < 0) n = (int)wcslen(s);
    W text(s, s + n);
    if (flags & DT_EXPANDTABS) {
        W t2;
        for (wchar_t c : text) { if (c == L'\t') t2 += L"    "; else t2 += c; }
        text = t2;
    }
    tf_hgdi_s *fo = cur_font(dc);
    Face *f = first_face_with(fo, 'n', nullptr);
    int asc = (int)fo->px, desc = 4;
    if (f) { float sc = font_scale(f, fo->px); asc = (int)(f->ascent * sc + 0.5f); desc = -(int)(f->descent * sc + 0.5f); }
    int lineh = asc + desc;
    int maxw = r->right - r->left;
    std::vector<W> words;
    { W cur; for (wchar_t c : text) {
        if (c == L' ') { words.push_back(cur); words.push_back(L" "); cur.clear(); }
        else if (c == L'\n') { words.push_back(cur); words.push_back(L"\n"); cur.clear(); }
        else if (c >= 0x2E80) { words.push_back(cur); W one; one += c; words.push_back(one); cur.clear(); }
        else cur += c; } words.push_back(cur); }
    std::vector<W> lines;
    if (flags & DT_WORDBREAK) {
        W cur;
        for (auto &wd : words) {
            if (wd == L"\n") { lines.push_back(cur); cur.clear(); continue; }
            if (wd == L" " && cur.empty()) continue;
            W cand = cur + wd;
            SIZE sz; GetTextExtentPoint32W(h, cand.c_str(), (int)cand.size(), &sz);
            if (sz.cx > maxw && !cur.empty()) { lines.push_back(cur); cur.clear(); if (wd != L" ") cur = wd; }
            else cur = cand;
        }
        lines.push_back(cur);
    } else lines.push_back(text);
    int wmax = 0;
    for (auto &l : lines) { SIZE sz; GetTextExtentPoint32W(h, l.c_str(), (int)l.size(), &sz); wmax = std::max<int>(wmax, sz.cx); }
    if (flags & DT_CALCRECT) {
        r->right = r->left + wmax;
        r->bottom = r->top + (int)lines.size() * lineh;
        return (int)lines.size() * lineh;
    }
    int totalh = (int)lines.size() * lineh;
    int y0 = r->top;
    if (flags & DT_VCENTER) y0 = r->top + ((r->bottom - r->top) - totalh) / 2;
    if (flags & DT_BOTTOM) y0 = r->bottom - totalh;
    for (size_t li = 0; li < lines.size(); li++) {
        const W &l = lines[li];
        SIZE sz; GetTextExtentPoint32W(h, l.c_str(), (int)l.size(), &sz);
        int x0 = r->left;
        if (flags & DT_CENTER) x0 = r->left + (r->right - r->left - sz.cx) / 2;
        else if (flags & DT_RIGHT) x0 = r->right - sz.cx;
        W draw = l;
        if ((flags & DT_END_ELLIPSIS) && sz.cx > maxw && maxw > 8) {
            SIZE es; GetTextExtentPoint32W(h, L"...", 3, &es);
            while (!draw.empty()) {
                draw.pop_back();
                SIZE ds; GetTextExtentPoint32W(h, draw.c_str(), (int)draw.size(), &ds);
                if (ds.cx + es.cx <= maxw) break;
            }
            draw += L"...";
        }
        int px = x0, py = y0 + (int)li * lineh + asc;   /* draw_glyph adds the DC origin itself */
        for (wchar_t c : draw) { draw_glyph(dc, fo, c, px, py, dc->text); px += glyph_metrics(fo, c).adv; }
    }
    touch(dc);
    return totalh;
}
int DrawTextA(HDC h, LPCSTR s, int n, LPRECT r, UINT f) {
    if (!s) return DrawTextW(h, L"", 0, r, f);
    if (n < 0) n = (int)strlen(s);
    extern W tf_u2w(const char *, bool);
    W w = tf_u2w(std::string(s, n).c_str(), false);
    return DrawTextW(h, w.c_str(), (int)w.size(), r, f);
}

/*GetGlyphOutlineW: the PSP system font (hle_font.cpp) rasterizes through here*/
DWORD GetGlyphOutlineW(HDC h, UINT code, UINT format, LPGLYPHMETRICS gm, DWORD size, LPVOID buf, const MAT2 *) {
    tf_hdc_s *dc = DC(h);
    if (!dc || !gm) return GDI_ERROR;
    tf_hgdi_s *fo = cur_font(dc);
    GM g = glyph_metrics(fo, (int)code);
    gm->gmBlackBoxX = std::max(0, g.x1 - g.x0);
    gm->gmBlackBoxY = std::max(0, g.y1 - g.y0);
    gm->gmptGlyphOrigin.x = g.x0;
    gm->gmptGlyphOrigin.y = -g.y0;                     /* GDI: y up from the baseline */
    gm->gmCellIncX = (short)(g.adv ? g.adv : (fo->px > 8 ? (int)fo->px / 2 : 4));
    gm->gmCellIncY = 0;
    if (format == GGO_METRICS) return 1;
    if (format != GGO_GRAY8_BITMAP) return GDI_ERROR;
    int w = gm->gmBlackBoxX, hh = gm->gmBlackBoxY;
    uint32_t pitch = (w + 3) & ~3u;                    /* GDI aligns GRAY8 rows to 4 bytes */
    DWORD need = pitch * hh;
    if (!buf || size < need) return need;
    if (!g.f || w <= 0 || hh <= 0) return 1;
    float sc = font_scale(g.f, fo->px);
    std::vector<uint8_t> cov((size_t)w * hh);
    if (g.glyph) stbtt_MakeGlyphBitmap(&g.f->info, cov.data(), w, hh, w, sc, sc, g.glyph);
    else stbtt_MakeCodepointBitmap(&g.f->info, cov.data(), w, hh, w, sc, sc, (int)code);
    memset(buf, 0, need);
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < w; x++)
            ((uint8_t *)buf)[y * pitch + x] = (uint8_t)(cov[(size_t)y * w + x] * 64 / 255);   /* GDI gray levels: 0..64 */
    return need;
}

/* ------------------------------------------------------------------ exports for shim_gdiplus (bitmap-target Graphics) */
extern "C++" uint32_t *tf_gdi_canvas_of(HDC hh, Canvas **cv, int *orgx, int *orgy, int *cw, int *ch,
                                        int *clipx, int *clipy, int *clipr, int *clipb) {
    tf_hdc_s *dc = DC(hh);
    if (!dc || !dc->cv) return nullptr;
    if (cv) *cv = dc->cv;
    if (orgx) *orgx = dc->orgx;
    if (orgy) *orgy = dc->orgy;
    if (cw) *cw = dc->cv->w;
    if (ch) *ch = dc->cv->h;
    if (clipx) *clipx = dc->clip.left;
    if (clipy) *clipy = dc->clip.top;
    if (clipr) *clipr = dc->clip.right;
    if (clipb) *clipb = dc->clip.bottom;
    return dc->cv->px;
}
extern "C++" int tf_gdi_text_width(HDC hh, const wchar_t *s, int n) {
    tf_hdc_s *dc = DC(hh);
    if (!dc) return 0;
    if (n < 0) n = (int)wcslen(s ? s : L"");
    tf_hgdi_s *fo = cur_font(dc);
    int x = 0;
    for (int i = 0; i < n; i++) x += glyph_metrics(fo, s[i]).adv;
    return x;
}
extern "C++" void tf_gdi_text_height(HDC hh, int *asc, int *desc) {
    tf_hdc_s *dc = DC(hh);
    if (!dc || !asc) return;
    tf_hgdi_s *fo = cur_font(dc);
    Face *f = first_face_with(fo, 'n', nullptr);
    int a = (int)fo->px, d = 4;
    if (f) { float sc = font_scale(f, fo->px); a = (int)(f->ascent * sc + 0.5f); d = -(int)(f->descent * sc + 0.5f); }
    *asc = a;
    if (desc) *desc = d;
}
/*draw with the TOP of the line at y_top; argb = 0xAARRGGBB (the alpha is applied by the caller's blend)*/
extern "C++" void tf_gdi_draw_text(HDC hh, const wchar_t *s, int n, int x, int y_top, uint32_t argb) {
    tf_hdc_s *dc = DC(hh);
    if (!dc || !s) return;
    if (n < 0) n = (int)wcslen(s);
    tf_hgdi_s *fo = cur_font(dc);
    Face *f = first_face_with(fo, 'n', nullptr);
    int asc = (int)fo->px;
    if (f) asc = (int)(f->ascent * font_scale(f, fo->px) + 0.5f);
    COLORREF col = RGB((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
    COLORREF old = dc->text;
    dc->text = col;
    int px = x, py = y_top + asc;
    for (int i = 0; i < n; i++) { draw_glyph(dc, fo, s[i], px, py, col); px += glyph_metrics(fo, s[i]).adv; }
    dc->text = old;
    touch(dc);
}
