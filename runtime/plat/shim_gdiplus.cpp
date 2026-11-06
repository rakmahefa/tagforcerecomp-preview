/* shim_gdiplus.cpp - the GDI+ compatibility shim implementation (see gdiplus.h).
 * Shapes are rendered with 2x2 coverage supersampling (antialiased like GDI+), text goes
 * through the GDI font stack (stb_truetype, AA glyphs), images decode with stb_image.
 * All drawing lands on the same ARGB canvas the GDI half uses (shim_gdi.cpp).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"
#include "gdiplus.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>

using namespace Gdiplus;
typedef std::wstring W;
std::string tf_w2u(const wchar_t *, bool);   /* shim_kern.cpp */

/*direct canvas access + text helpers from shim_gdi.cpp*/
extern "C++" {
struct Canvas;
struct tf_hdc_s;
uint32_t *tf_gdi_canvas_of(HDC dc, Canvas **cv, int *orgx, int *orgy, int *cw, int *ch, int *clipx, int *clipy, int *clipr, int *clipb);
int tf_gdi_text_width(HDC dc, const wchar_t *s, int n);
void tf_gdi_text_height(HDC dc, int *asc, int *desc);
void tf_gdi_draw_text(HDC dc, const wchar_t *s, int n, int x, int y_top, uint32_t argb);
}

/*draw text into a raw Ctx (bitmap targets): declared after Ctx, defined near DrawString*/

/* ------------------------------------------------------------------ Graphics */
Graphics::Graphics(HDC d) : dc(d) {}
Graphics::Graphics(Image *im) : dc(nullptr), img(im) {}
Graphics::~Graphics() {}

static void px_blend(uint32_t *dst, uint32_t src, uint32_t cov) {
    /*straight-alpha OVER (same math as modbox's misturar): correct for transparent bitmaps,
     *degenerates to plain replacement on the opaque window canvas*/
    uint32_t sal = ((src >> 24) & 255) * cov / 255;
    if (!sal) return;
    uint32_t da = (*dst >> 24) & 255;
    uint32_t oa = sal + da * (255 - sal) / 255;
    if (!oa) { *dst = 0; return; }
    uint32_t sr = (src >> 16) & 255, sg = (src >> 8) & 255, sb = src & 255;
    uint32_t dr = (*dst >> 16) & 255, dg = (*dst >> 8) & 255, db = *dst & 255;
    uint32_t ws = sal * 255, wd = da * (255 - sal), q = oa * 255;
    *dst = (oa << 24) | ((((sr * ws + dr * wd) / q) << 16)) | (((sg * ws + dg * wd) / q) << 8) | ((sb * ws + db * wd) / q);
}
struct Ctx {   /*where and how to draw right now*/
    uint32_t *px; int w, h;
    int ox, oy;
    int cx0, cy0, cx1, cy1;   /*clip in canvas coords*/
    bool aa;
};
static void draw_text_into(Ctx &c, const wchar_t *s, int n, int x, int y_top, uint32_t argb, HDC fontdc, HFONT font);
static Ctx ctx_of(Graphics *g) {
    Ctx c{};
    if (g->img_target() && g->img_target()->px) {   /* off-screen Bitmap target */
        c.px = g->img_target()->px; c.w = g->img_target()->w; c.h = g->img_target()->h;
        c.ox = c.oy = 0;
        c.cx0 = 0; c.cy0 = 0; c.cx1 = c.w; c.cy1 = c.h;
        if (g->has_clip()) {
            const RectF &cl = g->clip_rect();
            c.cx0 = std::max(0, (int)cl.x); c.cy0 = std::max(0, (int)cl.y);
            c.cx1 = std::min(c.w, (int)(cl.x + cl.Width)); c.cy1 = std::min(c.h, (int)(cl.y + cl.Height));
        }
        c.aa = g->smoothing();
        return c;
    }
    Canvas *cv = nullptr;
    c.px = tf_gdi_canvas_of(g->dc_h(), &cv, &c.ox, &c.oy, &c.w, &c.h, &c.cx0, &c.cy0, &c.cx1, &c.cy1);
    if (g->has_clip()) {
        const RectF &cl = g->clip_rect();
        c.cx0 = std::max(c.cx0, (int)cl.x + c.ox);
        c.cy0 = std::max(c.cy0, (int)cl.y + c.oy);
        c.cx1 = std::min(c.cx1, (int)(cl.x + cl.Width) + c.ox);
        c.cy1 = std::min(c.cy1, (int)(cl.y + cl.Height) + c.oy);
    }
    c.aa = g->smoothing();
    return c;
}

Status Graphics::Clear(const Color &c) {
    Ctx x = ctx_of(this);
    if (!x.px) return GenericError;
    uint32_t v = c.GetValue();   /* keep the alpha: modbox clears its panel bitmap to transparent */
    for (int y = x.cy0; y < x.cy1; y++)
        for (int xx = x.cx0; xx < x.cx1; xx++) x.px[(size_t)y * x.w + xx] = v;
    return Ok;
}

static float minx_of(const PointF *pts, int n) { float m = pts[0].x; for (int i = 1; i < n; i++) m = std::min(m, pts[i].x); return m; }
static float maxx_of(const PointF *pts, int n) { float m = pts[0].x; for (int i = 1; i < n; i++) m = std::max(m, pts[i].x); return m; }
/*polygon fill with coverage: 2x2 subsamples when AA, one sample otherwise*/
static void fill_poly(Ctx &x, const PointF *pts, int n, uint32_t argb) {
    if (!x.px || n < 3) return;
    float miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) { miny = std::min(miny, pts[i].y); maxy = std::max(maxy, pts[i].y); }
    const int SS = x.aa ? 2 : 1, SS2 = SS * SS;
    int y0 = (int)std::floor(miny) - 1 + x.oy, y1 = (int)std::ceil(maxy) + 1 + x.oy;
    for (int y = std::max(y0, x.cy0); y < std::min(y1, x.cy1); y++) {
        for (int xx = std::max((int)std::floor(minx_of(pts, n)) - 1 + x.ox, x.cx0); xx < x.cx1; xx++) {
            int cov = 0;
            for (int sy = 0; sy < SS; sy++) for (int sx = 0; sx < SS; sx++) {
                float px = xx - x.ox + (sx + 0.5f) / SS, py = y - x.oy + (sy + 0.5f) / SS;
                bool inside = false;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    if ((pts[i].y > py) != (pts[j].y > py) &&
                        px < (pts[j].x - pts[i].x) * (py - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
                        inside = !inside;
                }
                if (inside) cov++;
            }
            if (cov) px_blend(&x.px[(size_t)y * x.w + xx], argb, (uint32_t)((argb >> 24) * cov / SS2));
        }
    }
}

/*stroke a polyline with a width (AA: coverage by sample distance)*/
static void stroke_poly(Ctx &x, const PointF *pts, int n, uint32_t argb, float width) {
    if (!x.px || n < 2) return;
    float half = std::max(0.55f, width / 2.0f), reach = half + 1.0f;
    float miny = pts[0].y, maxy = pts[0].y, minxx = pts[0].x, maxxx = pts[0].x;
    for (int i = 1; i < n; i++) { miny = std::min(miny, pts[i].y); maxy = std::max(maxy, pts[i].y); minxx = std::min(minxx, pts[i].x); maxxx = std::max(maxxx, pts[i].x); }
    for (int y = std::max((int)std::floor(miny - reach) + x.oy, x.cy0); y < std::min((int)std::ceil(maxy + reach) + x.oy, x.cy1); y++)
        for (int xx = std::max((int)std::floor(minxx - reach) + x.ox, x.cx0); xx < std::min((int)std::ceil(maxxx + reach) + x.ox, x.cx1); xx++) {
            float best = 1e9f;
            float px = xx - x.ox + 0.5f, py = y - x.oy + 0.5f;
            for (int i = 1; i < n; i++) {
                float ax = pts[i - 1].x, ay = pts[i - 1].y, bx = pts[i].x, by = pts[i].y;
                float dx = bx - ax, dy = by - ay;
                float len2 = dx * dx + dy * dy;
                float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
                t = std::max(0.0f, std::min(1.0f, t));
                float qx = ax + t * dx - px, qy = ay + t * dy - py;
                best = std::min(best, sqrtf(qx * qx + qy * qy));
            }
            if (best < half) px_blend(&x.px[(size_t)y * x.w + xx], argb, 255);
            else if (x.aa && best < half + 1.0f) px_blend(&x.px[(size_t)y * x.w + xx], argb, (uint32_t)(255 * (half + 1.0f - best)));
        }
}

static uint32_t grad_color(const LinearGradientBrush *b, float px) {
    /*project the point on the gradient axis (x-only gradients in this codebase's use)*/
    float t = (px - b->a.x) / std::max(0.01f, b->b.x - b->a.x);
    t = std::max(0.0f, std::min(1.0f, t));
    if (!b->ints.empty() && b->poss.size() == b->ints.size()) {
        for (size_t i = 0; i < b->poss.size() - 1; i++)
            if (t >= b->poss[i] && t <= b->poss[i + 1]) {
                float u = (t - b->poss[i]) / std::max(1e-6f, b->poss[i + 1] - b->poss[i]);
                uint32_t c0 = b->ints[i].GetValue(), c1 = b->ints[i + 1].GetValue();
                uint32_t out = 0;
                for (int sh = 24; sh >= 0; sh -= 8) {
                    uint32_t v0 = (c0 >> sh) & 0xFF, v1 = (c1 >> sh) & 0xFF;
                    out |= (uint32_t)(v0 + (v1 - v0) * u) << sh;
                }
                return out;
            }
        return b->ints.back().GetValue();
    }
    uint32_t c0 = b->ca.GetValue(), c1 = b->cb.GetValue();
    uint32_t out = 0;
    for (int sh = 24; sh >= 0; sh -= 8) {
        uint32_t v0 = (c0 >> sh) & 0xFF, v1 = (c1 >> sh) & 0xFF;
        out |= (uint32_t)(v0 + (v1 - v0) * t) << sh;
    }
    return out;
}

Status Graphics::SetClip(const GraphicsPath *path) {
    if (!path || path->contours.empty()) { have_clip = false; return Ok; }
    REAL x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (auto &c : path->contours) for (auto &pt : c) { x0 = std::min(x0, pt.x); y0 = std::min(y0, pt.y); x1 = std::max(x1, pt.x); y1 = std::max(y1, pt.y); }
    clip = RectF(x0, y0, x1 - x0, y1 - y0);
    have_clip = true;
    return Ok;
}
Status Graphics::FillRectangle(const Brush *b, REAL x, REAL y, REAL w, REAL h) {
    Ctx c = ctx_of(this);
    if (!c.px || !b) return GenericError;
    if (const LinearGradientBrush *g = dynamic_cast<const LinearGradientBrush *>(b)) {
        for (int yy = std::max((int)y + c.oy, c.cy0); yy < std::min((int)(y + h) + c.oy + 1, c.cy1); yy++)
            for (int xx = std::max((int)x + c.ox, c.cx0); xx < std::min((int)(x + w) + c.ox + 1, c.cx1); xx++)
                px_blend(&c.px[(size_t)yy * c.w + xx], grad_color(g, xx - c.ox), 255);
        return Ok;
    }
    uint32_t col = dynamic_cast<const SolidBrush *>(b) ? dynamic_cast<const SolidBrush *>(b)->col.GetValue() : 0xFF202020;
    for (int yy = std::max((int)std::floor(y) + c.oy, c.cy0); yy < std::min((int)std::ceil(y + h) + c.oy, c.cy1); yy++)
        for (int xx = std::max((int)std::floor(x) + c.ox, c.cx0); xx < std::min((int)std::ceil(x + w) + c.ox, c.cx1); xx++)
            px_blend(&c.px[(size_t)yy * c.w + xx], col, 255);
    return Ok;
}
Status Graphics::FillEllipse(const Brush *b, REAL x, REAL y, REAL w, REAL h) {
    Ctx c = ctx_of(this);
    if (!c.px || !b) return GenericError;
    uint32_t col = dynamic_cast<const SolidBrush *>(b) ? dynamic_cast<const SolidBrush *>(b)->col.GetValue() : 0xFF202020;
    const int SS = c.aa ? 2 : 1, SS2 = SS * SS;
    float cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
    for (int yy = std::max((int)y + c.oy, c.cy0); yy < std::min((int)(y + h) + c.oy + 1, c.cy1); yy++)
        for (int xx = std::max((int)x + c.ox, c.cx0); xx < std::min((int)(x + w) + c.ox + 1, c.cx1); xx++) {
            int cov = 0;
            for (int sy = 0; sy < SS; sy++) for (int sx = 0; sx < SS; sx++) {
                float px = xx - c.ox + (sx + 0.5f) / SS, py = yy - c.oy + (sy + 0.5f) / SS;
                float dx = (px - cx) / rx, dy = (py - cy) / ry;
                if (dx * dx + dy * dy <= 1.0f) cov++;
            }
            if (cov) px_blend(&c.px[(size_t)yy * c.w + xx], col, (uint32_t)((col >> 24) * cov / SS2));
        }
    return Ok;
}
Status Graphics::FillPolygon(const Brush *b, const PointF *pts, INT n) {
    Ctx c = ctx_of(this);
    if (!c.px || !b) return GenericError;
    uint32_t col = dynamic_cast<const SolidBrush *>(b) ? dynamic_cast<const SolidBrush *>(b)->col.GetValue() : 0xFF202020;
    fill_poly(c, pts, n, col);
    return Ok;
}
Status Graphics::FillPath(const Brush *b, const GraphicsPath *p) {
    Ctx c = ctx_of(this);
    if (!c.px || !b || !p) return GenericError;
    for (auto &contour : p->contours) {
        if (contour.size() < 3) continue;
        if (const LinearGradientBrush *g = dynamic_cast<const LinearGradientBrush *>(b)) {
            /*per-pixel gradient fill for the accent line*/
            float miny = contour[0].y, maxy = contour[0].y, minxx = minx_of(contour.data(), (int)contour.size()), maxxx = maxx_of(contour.data(), (int)contour.size());
            for (auto &pt : contour) { miny = std::min(miny, pt.y); maxy = std::max(maxy, pt.y); }
            for (int yy = std::max((int)miny + c.oy, c.cy0); yy < std::min((int)maxy + c.oy + 1, c.cy1); yy++)
                for (int xx = std::max((int)minxx + c.ox, c.cx0); xx < std::min((int)maxxx + c.ox + 1, c.cx1); xx++) {
                    float px = xx - c.ox + 0.5f, py = yy - c.oy + 0.5f;
                    bool inside = false;
                    for (size_t i = 0, j = contour.size() - 1; i < contour.size(); j = i++)
                        if ((contour[i].y > py) != (contour[j].y > py) &&
                            px < (contour[j].x - contour[i].x) * (py - contour[i].y) / (contour[j].y - contour[i].y) + contour[i].x)
                            inside = !inside;
                    if (inside) px_blend(&c.px[(size_t)yy * c.w + xx], grad_color(g, px), 255);
                }
        } else {
            uint32_t col = dynamic_cast<const SolidBrush *>(b) ? dynamic_cast<const SolidBrush *>(b)->col.GetValue() : 0xFF202020;
            fill_poly(c, contour.data(), (int)contour.size(), col);
        }
    }
    return Ok;
}
Status Graphics::DrawPath(const Pen *p, const GraphicsPath *path) {
    Ctx c = ctx_of(this);
    if (!c.px || !p || !path) return GenericError;
    for (auto &contour : path->contours)
        if (contour.size() >= 2) stroke_poly(c, contour.data(), (int)contour.size(), p->col.GetValue(), p->width);
    return Ok;
}
Status Graphics::DrawPolygon(const Pen *p, const PointF *pts, INT n) {
    Ctx c = ctx_of(this);
    if (!c.px || !p) return GenericError;
    std::vector<PointF> closed(pts, pts + n);
    if (n) closed.push_back(pts[0]);
    stroke_poly(c, closed.data(), (int)closed.size(), p->col.GetValue(), p->width);
    return Ok;
}
Status Graphics::DrawEllipse(const Pen *p, REAL x, REAL y, REAL w, REAL h) {
    GraphicsPath path;
    path.AddArc(x, y, w, h, 0, 360);
    return DrawPath(p, &path);
}

/* ------------------------------------------------------------------ text */
static HFONT font_of(Graphics *g, const Font *f) {
    if (!f) return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int weight = (f->GetStyle() & FontStyleBold) ? FW_SEMIBOLD : FW_NORMAL;
    return CreateFontW(-(int)(f->px + 0.5f), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, f->fam ? f->fam->name.c_str() : L"Segoe UI");
}
/* text on a bitmap target: the GDI font stack needs an HDC; use a scratch memory DC */
static HDC scratch_dc_for_font() {
    static HDC scratch = nullptr;
    if (!scratch) {
        extern HDC tf_gdi_dc_for_canvas(struct Canvas *, int, int);
        extern struct Canvas *tf_gdi_new_canvas(int, int);
        scratch = tf_gdi_dc_for_canvas(tf_gdi_new_canvas(4, 4), 0, 0);
    }
    return scratch;
}
Status Graphics::MeasureString(const wchar_t *s, INT n, const Font *font, const PointF &, const StringFormat *fmt, RectF *box) const {
    if (!s) { if (box) *box = RectF(); return Ok; }
    if (n < 0) n = (int)wcslen(s);
    HDC target = dc ? dc : scratch_dc_for_font();
    HFONT f = font_of(nullptr, font), of = (HFONT)SelectObject(target, f);
    int wd = tf_gdi_text_width(target, s, n);
    int asc = 0, desc = 0;
    tf_gdi_text_height(target, &asc, &desc);
    SelectObject(target, of);
    DeleteObject(f);
    (void)fmt;
    if (box) *box = RectF(0, 0, (REAL)wd, (REAL)(asc + desc));
    return Ok;
}
Status Graphics::MeasureString(const wchar_t *s, INT n, const Font *font, const RectF &layout, const StringFormat *fmt, RectF *box, INT *lines, INT *codepoints) const {
    if (lines) *lines = 1;
    if (codepoints) *codepoints = n < 0 ? (INT)wcslen(s) : n;
    Status st = MeasureString(s, n, font, PointF(), fmt, box);
    if (box && !(fmt && (fmt->flags & StringFormatFlagsNoWrap)) && box->Width > layout.Width) box->Width = layout.Width;   /*wraps to the layout width*/
    return st;
}
Status Graphics::DrawString(const wchar_t *s, INT n, const Font *font, const PointF &origin, const StringFormat *fmt, const Brush *b) {
    if (!s) return Ok;
    if (n < 0) n = (int)wcslen(s);
    uint32_t col = b && dynamic_cast<const SolidBrush *>(b) ? dynamic_cast<const SolidBrush *>(b)->col.GetValue() : 0xFFFFFFFF;
    HDC target = dc ? dc : scratch_dc_for_font();
    HFONT f = font_of(this, font), of = (HFONT)SelectObject(target, f);
    int wd = tf_gdi_text_width(target, s, n);
    int asc = 0, desc = 0;
    tf_gdi_text_height(target, &asc, &desc);
    float x = origin.x;
    if (fmt && fmt->align == StringAlignmentCenter) x = origin.x - wd / 2.0f;
    else if (fmt && fmt->align == StringAlignmentFar) x = origin.x - wd;
    if (dc) {
        tf_gdi_draw_text(dc, s, n, (int)(x + 0.5f), (int)(origin.y + 0.5f), col);
    } else {   /* bitmap target: rasterize the glyphs straight into it (with the AA of stb) */
        Ctx c = ctx_of(this);
        if (c.px) draw_text_into(c, s, n, (int)(x + 0.5f), (int)(origin.y + 0.5f), col, target, f);
    }
    SelectObject(target, of);
    DeleteObject(f);
    return Ok;
}
Status Graphics::DrawString(const wchar_t *s, INT n, const Font *font, const RectF &layout, const StringFormat *fmt, const Brush *b) {
    if (!s) return Ok;
    if (n < 0) n = (int)wcslen(s);
    uint32_t col = b && dynamic_cast<const SolidBrush *>(b) ? dynamic_cast<const SolidBrush *>(b)->col.GetValue() : 0xFFFFFFFF;
    HDC target = dc ? dc : scratch_dc_for_font();
    HFONT f = font_of(this, font), of = (HFONT)SelectObject(target, f);
    int wd = tf_gdi_text_width(target, s, n);
    int asc = 0, desc = 0;
    tf_gdi_text_height(target, &asc, &desc);
    std::wstring t(s, s + n);
    if (fmt && fmt->trimming == StringTrimmingEllipsisCharacter && wd > layout.Width && layout.Width > 12) {
        int ew = tf_gdi_text_width(target, L"...", 3);
        while (!t.empty() && tf_gdi_text_width(target, t.c_str(), (int)t.size()) + ew > layout.Width) t.pop_back();
        t += L"...";
        wd = tf_gdi_text_width(target, t.c_str(), (int)t.size());
    }
    float x = layout.x;
    if (fmt && fmt->align == StringAlignmentCenter) x = layout.x + (layout.Width - wd) / 2;
    else if (fmt && fmt->align == StringAlignmentFar) x = layout.x + layout.Width - wd;
    float y = layout.y;
    if (fmt && fmt->linealign == StringAlignmentCenter) y = layout.y + (layout.Height - (asc + desc)) / 2;
    else if (fmt && fmt->linealign == StringAlignmentFar) y = layout.y + layout.Height - (asc + desc);
    if (dc) {
        tf_gdi_draw_text(dc, t.c_str(), (int)t.size(), (int)(x + 0.5f), (int)(y + 0.5f), col);
    } else {
        Ctx c = ctx_of(this);
        if (c.px) draw_text_into(c, t.c_str(), (int)t.size(), (int)(x + 0.5f), (int)(y + 0.5f), col, target, f);
    }
    SelectObject(target, of);
    DeleteObject(f);
    return Ok;
}

/*draw text into a raw Ctx (bitmap targets): glyph coverage via GetGlyphOutlineW on the font DC*/
static void draw_text_into(Ctx &c, const wchar_t *s, int n, int x, int y_top, uint32_t argb, HDC fontdc, HFONT) {
    if (!s || !c.px) return;
    GLYPHMETRICS gm{};
    MAT2 m = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    int penx = x;
    for (int i = 0; i < n; i++) {
        DWORD sz = GetGlyphOutlineW(fontdc, (UINT)s[i], GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        if (sz != GDI_ERROR && sz > 0) {
            std::vector<uint8_t> buf(sz);
            if (GetGlyphOutlineW(fontdc, (UINT)s[i], GGO_GRAY8_BITMAP, &gm, sz, buf.data(), &m) != GDI_ERROR) {
                const uint32_t pitch = (gm.gmBlackBoxX + 3) & ~3u;
                for (uint32_t gy = 0; gy < gm.gmBlackBoxY; gy++) {
                    int dy = y_top + gm.gmptGlyphOrigin.y + (int)gy;
                    if (dy < c.cy0 || dy >= c.cy1) continue;
                    for (uint32_t gx = 0; gx < gm.gmBlackBoxX; gx++) {
                        int dx = penx + gm.gmptGlyphOrigin.x + (int)gx;
                        if (dx < c.cx0 || dx >= c.cx1) continue;
                        uint32_t v = buf[gy * pitch + gx];
                        if (v) px_blend(&c.px[(size_t)dy * c.w + dx], argb, v * 255 / 64);
                    }
                }
            }
        }
        penx += gm.gmCellIncX ? gm.gmCellIncX : 8;
    }
}

/* ------------------------------------------------------------------ images */
Status Graphics::DrawImage(Image *img, INT x, INT y, INT w, INT h) {
    Ctx c = ctx_of(this);
    if (!c.px || !img || !img->px || w <= 0 || h <= 0) return GenericError;
    int sw = img->w, sh = img->h;
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy + c.oy;
        if (dy < c.cy0 || dy >= c.cy1) continue;
        float syy = (yy + 0.5f) * sh / h - 0.5f;
        int sy0 = (int)syy, fy = (int)((syy - sy0) * 256);
        if (sy0 < 0) { sy0 = 0; fy = 0; }
        if (sy0 >= sh - 1) { sy0 = sh - 1; fy = 0; }
        for (int xx = 0; xx < w; xx++) {
            int dx = x + xx + c.ox;
            if (dx < c.cx0 || dx >= c.cx1) continue;
            float sxx = (xx + 0.5f) * sw / w - 0.5f;
            int sx0 = (int)sxx, fx = (int)((sxx - sx0) * 256);
            if (sx0 < 0) { sx0 = 0; fx = 0; }
            if (sx0 >= sw - 1) { sx0 = sw - 1; fx = 0; }
            const uint32_t *p00 = &img->px[(size_t)sy0 * sw + sx0], *p10 = p00 + (sx0 + 1 < sw ? 1 : 0);
            const uint32_t *p01 = &img->px[(size_t)(sy0 + 1 < sh ? sy0 + 1 : sy0) * sw + sx0], *p11 = p01 + (sx0 + 1 < sw ? 1 : 0);
            uint32_t out = 0xFF000000u;
            for (int sh2 = 0; sh2 < 24; sh2 += 8) {
                uint32_t top = ((p00[0] >> sh2) & 255) * (256 - fx) + ((p10[0] >> sh2) & 255) * fx;
                uint32_t bot = ((p01[0] >> sh2) & 255) * (256 - fx) + ((p11[0] >> sh2) & 255) * fx;
                out |= ((top * (256 - fy) + bot * fy) >> 16) << sh2;
            }
            uint32_t a = (p00[0] >> 24) * (256 - fx) * (256 - fy) / 65536 + ((p00[0] >> 24) * fx * (256 - fy) + (p01[0] >> 24) * (256 - fx) * fy + (p11[0] >> 24) * fx * fy) / 65536;
            px_blend(&c.px[(size_t)dy * c.w + dx], out | ((uint32_t)a << 24), 255);
        }
    }
    return Ok;
}
Status Graphics::DrawImage(Image *img, const RectF &dest) { return DrawImage(img, (INT)dest.x, (INT)dest.y, (INT)dest.Width, (INT)dest.Height); }

/* ------------------------------------------------------------------ GraphicsPath */
Status GraphicsPath::AddRectangle(const RectF &r) {
    std::vector<PointF> c = {PointF(r.x, r.y), PointF(r.x + r.Width, r.y), PointF(r.x + r.Width, r.y + r.Height), PointF(r.x, r.y + r.Height), PointF(r.x, r.y)};
    contours.push_back(std::move(c));
    return Ok;
}
Status GraphicsPath::AddArc(REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep) {
    /*GDI+ AddArc appends to the current figure; we approximate with a polyline (36 steps per 90 degrees)*/
    REAL rx = w / 2, ry = h / 2, cx = x + rx, cy = y + ry;
    if (contours.empty()) contours.emplace_back();
    auto &c = contours.back();
    const int steps = std::max(4, (int)(sweep / 5.0f));
    for (int i = 0; i <= steps; i++) {
        REAL a = (start + sweep * i / steps) * 3.14159265f / 180.0f;
        c.push_back(PointF(cx + rx * cosf(a), cy + ry * sinf(a)));
    }
    return Ok;
}
Status GraphicsPath::AddBezier(REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4) {
    if (contours.empty()) contours.emplace_back();
    auto &c = contours.back();
    for (int i = 0; i <= 16; i++) {
        float t = i / 16.0f, u = 1 - t;
        c.push_back(PointF(u * u * u * x1 + 3 * u * u * t * x2 + 3 * u * t * t * x3 + t * t * t * x4,
                          u * u * u * y1 + 3 * u * u * t * y2 + 3 * u * t * t * y3 + t * t * t * y4));
    }
    return Ok;
}
Status GraphicsPath::AddLine(REAL x1, REAL y1, REAL x2, REAL y2) {
    if (contours.empty()) contours.emplace_back();
    contours.back().push_back(PointF(x2, y2));
    return Ok;
}

/* ------------------------------------------------------------------ Bitmap */
Bitmap::Bitmap(INT w, INT h, PixelFormatShim) {
    this->w = std::max(1, w); this->h = std::max(1, h);
    px = (uint32_t *)calloc((size_t)this->w * this->h, 4);
    status = px ? Ok : OutOfMemory;
}
Bitmap *Bitmap::FromFile(const wchar_t *path) {
    Bitmap *b = new Bitmap();
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(::tf_w2u(path, true).c_str(), &w, &h, &n, 4);
    if (!px) { b->status = FileNotFound; return b; }
    b->w = w; b->h = h;
    b->px = (uint32_t *)px;
    b->status = Ok;
    return b;
}
Bitmap *Bitmap::FromStream(IStream *stream) {
    Bitmap *b = new Bitmap();
    if (!stream) { b->status = InvalidParameter; return b; }
    /*read the whole stream*/
    std::vector<unsigned char> data;
    unsigned char buf[65536];
    for (;;) {
        ULONG got = 0;
        if (stream->Read(buf, sizeof buf, &got) != S_OK || !got) break;
        data.insert(data.end(), buf, buf + got);
    }
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!px) { b->status = GenericError; return b; }
    b->w = w; b->h = h;
    b->px = (uint32_t *)px;
    b->status = Ok;
    return b;
}
Status Bitmap::LockBits(const Rect *r, ImageLockMode, PixelFormatShim, BitmapData *out) {
    if (!r || !out) return InvalidParameter;
    out->Width = r->Width; out->Height = r->Height;
    out->Stride = r->Width * 4;
    out->Scan0 = px ? px + (size_t)r->y * w + r->x : nullptr;
    return out->Scan0 ? Ok : GenericError;
}
Status Bitmap::UnlockBits(BitmapData *) { return Ok; }

/* ------------------------------------------------------------------ IStream over memory (launcher's thumbnail) */
struct MemStream : IStream {
    HGLOBAL glob;
    BOOL del;
    size_t pos = 0, size = 0;
    ULONG Release() override {
        if (del && glob) GlobalFree(glob);
        delete this;
        return 0;
    }
    HRESULT Read(void *dst, ULONG n, ULONG *got) override {
        size_t take = std::min((size_t)n, size - pos);
        memcpy(dst, (const char *)glob + pos, take);
        pos += take;
        if (got) *got = (ULONG)take;
        return S_OK;
    }
    HRESULT Seek(LARGE_INTEGER d, DWORD from, ULARGE_INTEGER *np) override {
        size_t base = from == 0 ? 0 : from == 1 ? pos : size;
        long long p = (long long)base + d.QuadPart;
        if (p < 0) p = 0;
        if ((size_t)p > size) p = (long long)size;
        pos = (size_t)p;
        if (np) np->QuadPart = pos;
        return S_OK;
    }
    HRESULT Stat(void *, DWORD) override { return S_OK; }
};
HRESULT CreateStreamOnHGlobal(HGLOBAL glob, BOOL delete_on_release, IStream **out) {
    if (!glob || !out) return E_INVALIDARG;
    MemStream *m = new MemStream();
    m->glob = glob; m->del = delete_on_release;
    m->size = GlobalSize(glob);
    *out = m;
    return S_OK;
}
