/* gdiplus.Height - the GDI+ compatibility shim for the Linux build (see windows.Height).
 * Only the surface the runtime uses (modbox.cpp's in-game panels, launcher.cpp's thumbnail and
 * dark_ui.cpp's rounded corners): Graphics on an HDC with antialiasing, GraphicsPath with
 * arcs/beziers, SolidBrush / LinearGradientBrush, Pen, Font/FontFamily, StringFormat,
 * Bitmap (PNG from a memory stream) and DrawImage with high-quality scaling.
 */
#ifndef TF_GDIPLUS_SHIM_H
#define TF_GDIPLUS_SHIM_H
#include "windows.h"

#define STBTT_WINDOWS_DUMMY 0
#include <string>
#include <vector>
#include "objidl.h"

namespace Gdiplus {

typedef float REAL;
typedef int INT; typedef unsigned int UINT; typedef int BOOL_GD;   /* BOOL already taken by wingdi world */

enum Status { Ok = 0, GenericError = 1, InvalidParameter = 2, OutOfMemory = 3, FileNotFound = 10 };
enum SmoothingMode { SmoothingModeInvalid = -1, SmoothingModeDefault = 0, SmoothingModeAntiAlias = 4 };
enum PixelOffsetMode { PixelOffsetModeDefault = 0, PixelOffsetModeHalf = 4 };
enum TextRenderingHint { TextRenderingHintSystemDefault = 0, TextRenderingHintAntiAliasGridFit = 3 };
enum InterpolationMode { InterpolationModeDefault = 0, InterpolationModeHighQualityBicubic = 7 };
enum Unit { UnitWorld = 0, UnitDisplay = 1, UnitPixel = 2 };
enum FontStyle { FontStyleRegular = 0, FontStyleBold = 1, FontStyleItalic = 2 };
enum StringAlignment { StringAlignmentNear = 0, StringAlignmentCenter = 1, StringAlignmentFar = 2 };
enum StringTrimming { StringTrimmingNone = 0, StringTrimmingEllipsisCharacter = 1 };
enum StringFormatFlags { StringFormatFlagsDirectionRightToLeft = 1, StringFormatFlagsNoWrap = 0x1000, StringFormatFlagsMeasureTrailingSpaces = 0x2000 };
enum WrapMode { WrapModeTile = 0, WrapModeTileFlipX = 1, WrapModeTileFlipY = 2, WrapModeTileFlipXY = 3, WrapModeClamp = 4 };
enum ImageLockMode { ImageLockModeRead = 1 };
enum PixelFormatShim { PixelFormat32bppARGB = 0x26200A, PixelFormat32bppPARGB = 0x26200B };
#define PixelFormat32bppARGB Gdiplus::PixelFormat32bppARGB
#define PixelFormat32bppPARGB Gdiplus::PixelFormat32bppPARGB

struct PointF { REAL x, y; PointF() : x(0), y(0) {} PointF(REAL a, REAL b) : x(a), y(b) {} };
struct RectF { REAL x, y, Width, Height; RectF() : x(0), y(0), Width(0), Height(0) {} RectF(REAL a, REAL b, REAL c, REAL d) : x(a), y(b), Width(c), Height(d) {} };
struct Rect { INT x, y, Width, Height; Rect() : x(0), y(0), Width(0), Height(0) {} Rect(INT a, INT b, INT c, INT d) : x(a), y(b), Width(c), Height(d) {} };

class Color {
public:
    Color() : argb(0xFF000000) {}
    Color(BYTE a, BYTE r, BYTE g, BYTE b) : argb(((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b) {}
    Color(BYTE r, BYTE g, BYTE b) : argb(0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | b) {}
    explicit Color(DWORD v) : argb(v) {}
    DWORD GetValue() const { return argb; }
    BYTE GetAlpha() const { return (BYTE)(argb >> 24); }
    BYTE GetRed() const { return (BYTE)(argb >> 16); }
    BYTE GetGreen() const { return (BYTE)(argb >> 8); }
    BYTE GetBlue() const { return (BYTE)argb; }
private:
    DWORD argb;
};

class GraphicsPath;
class FontFamily;
class Font;
class StringFormat;
class Image;
class Brush;

class Brush {
public:
    virtual ~Brush() {}
};
class SolidBrush : public Brush {
public:
    explicit SolidBrush(const Color &c) : col(c) {}
    Color col;
};
class LinearGradientBrush : public Brush {
public:
    LinearGradientBrush(const PointF &p0, const PointF &p1, const Color &c0, const Color &c1) : a(p0), b(p1), ca(c0), cb(c1) {}
    Status SetInterpolationColors(const Color *colors, const REAL *positions, INT count) { ints.assign(colors, colors + count); poss.assign(positions, positions + count); return Ok; }
    Status SetWrapMode(WrapMode) { return Ok; }
    PointF a, b; Color ca, cb;
    std::vector<Color> ints; std::vector<REAL> poss;
};
class Pen {
public:
    Pen() : col(0, 0, 0), width(1.0f) {}
    Pen(const Color &c, REAL w = 1.0f) : col(c), width(w) {}
    Pen(const Brush *b, REAL w = 1.0f) : col(0, 0, 0), width(w) { if (b) { if (auto *s = dynamic_cast<const SolidBrush *>(b)) col = s->col; } }
    Status SetAlignment(int) { return Ok; }
    Color col; REAL width;
};

class Graphics {
public:
    explicit Graphics(HDC dc);
    explicit Graphics(Image *img);   /* draws onto a Bitmap (modbox renders its panels off-screen) */
    ~Graphics();
    HDC dc_h() const { return dc; }
    Image *img_target() const { return img; }
    bool smoothing() const { return smooth; }
    bool has_clip() const { return have_clip; }
    const RectF &clip_rect() const { return clip; }
    Status SetSmoothingMode(SmoothingMode m) { smooth = m == SmoothingModeAntiAlias; return Ok; }
    Status SetPixelOffsetMode(PixelOffsetMode) { return Ok; }
    Status SetTextRenderingHint(TextRenderingHint) { return Ok; }
    Status SetInterpolationMode(InterpolationMode) { return Ok; }
    Status Clear(const Color &c);
    Status SetClip(const RectF &r) { clip = r; have_clip = true; return Ok; }
    Status SetClip(const Rect &r) { clip = RectF((REAL)r.x, (REAL)r.y, (REAL)r.Width, (REAL)r.Height); have_clip = true; return Ok; }
    Status ResetClip() { have_clip = false; return Ok; }
    Status SetClip(const GraphicsPath *path);
    /*shapes: antialiased (coverage sampling) when smoothing is on, like GDI+*/
    Status FillRectangle(const Brush *b, REAL x, REAL y, REAL w, REAL h);
    Status FillRectangle(const Brush *b, const RectF &r) { return FillRectangle(b, r.x, r.y, r.Width, r.Height); }
    Status FillRectangle(const Brush *b, INT x, INT y, INT w, INT h) { return FillRectangle(b, (REAL)x, (REAL)y, (REAL)w, (REAL)h); }
    Status FillRectangle(const Brush *b, const Rect &r) { return FillRectangle(b, (REAL)r.x, (REAL)r.y, (REAL)r.Width, (REAL)r.Height); }
    Status FillEllipse(const Brush *b, REAL x, REAL y, REAL w, REAL h);
    Status FillEllipse(const Brush *b, const RectF &r) { return FillEllipse(b, r.x, r.y, r.Width, r.Height); }
    Status FillPolygon(const Brush *b, const PointF *pts, INT n);
    Status FillPath(const Brush *b, const GraphicsPath *p);
    Status DrawPath(const Pen *p, const GraphicsPath *path);
    Status DrawPolygon(const Pen *p, const PointF *pts, INT n);
    Status DrawEllipse(const Pen *p, REAL x, REAL y, REAL w, REAL h);
    /*text*/
    Status DrawString(const wchar_t *s, INT n, const Font *font, const PointF &origin, const StringFormat *fmt, const Brush *b);
    Status DrawString(const wchar_t *s, INT n, const Font *font, const RectF &layout, const StringFormat *fmt, const Brush *b);
    Status MeasureString(const wchar_t *s, INT n, const Font *font, const PointF &origin, const StringFormat *fmt, RectF *box) const;
    Status MeasureString(const wchar_t *s, INT n, const Font *font, const RectF &layout, const StringFormat *fmt, RectF *box, INT *lines = nullptr, INT *codepoints = nullptr) const;
    /*images*/
    Status DrawImage(class Image *img, INT x, INT y, INT w, INT h);
    Status DrawImage(class Image *img, const RectF &dest);
private:
    HDC dc = nullptr;
    Image *img = nullptr;
    bool smooth = true;
    RectF clip; bool have_clip = false;
};

class FontFamily {
public:
    FontFamily() : avail(false) {}
    FontFamily(const wchar_t *face) : avail(true), name(face ? face : L"") {}
    bool IsAvailable() const { return avail; }
    static const FontFamily *GenericSansSerif() { static FontFamily f(L"Segoe UI"); return &f; }
    static const FontFamily *GenericSerif() { static FontFamily f(L"Segoe UI"); return &f; }
    static const FontFamily *GenericMonospace() { static FontFamily f(L"Courier New"); return &f; }
    bool avail;
    std::wstring name;
};
class Font {
public:
    Font() : fam(nullptr), px(16), style(FontStyleRegular) {}
    Font(const FontFamily *f, REAL size, INT st = FontStyleRegular, Unit = UnitPixel) : fam(f), px(size), style(st) { if (!f) fam = FontFamily::GenericSansSerif(); }
    REAL GetSize() const { return px; }
    INT GetStyle() const { return style; }
    const FontFamily *fam;
    REAL px;
    INT style;
};

class StringFormat {
public:
    StringFormat() : flags(0), align(StringAlignmentNear), linealign(StringAlignmentNear), trimming(StringTrimmingNone) {}
    explicit StringFormat(const StringFormat *f) { if (f) *this = *f; }
    static const StringFormat *GenericTypographic() { static StringFormat f; return &f; }
    static const StringFormat *GenericDefault() { static StringFormat f; return &f; }
    StringFormat *Clone() const { auto *f = new StringFormat(*this); return f; }
    Status SetFormatFlags(INT f) { flags = f; return Ok; }
    INT GetFormatFlags() const { return flags; }
    Status SetAlignment(StringAlignment a) { align = a; return Ok; }
    Status SetLineAlignment(StringAlignment a) { linealign = a; return Ok; }
    Status SetTrimming(StringTrimming t) { trimming = t; return Ok; }
    INT flags; StringAlignment align, linealign; StringTrimming trimming;
};

/*GraphicsPath: arcs/beziers are flattened into polylines at Add time (rendering only needs fills/strokes)*/
class GraphicsPath {
public:
    GraphicsPath() {}
    Status AddRectangle(const RectF &r);
    Status AddArc(REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
    Status AddBezier(REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4);
    Status AddLine(REAL x1, REAL y1, REAL x2, REAL y2);
    Status CloseFigure() { closed = true; return Ok; }
    Status Reset() { contours.clear(); closed = false; return Ok; }
    /*the flattened contours (each a polyline in order)*/
    std::vector<std::vector<PointF>> contours;
    bool closed = false;
};

/*Bitmap: decoded with stb_image (PNG/JPEG from memory); LockBits hands out the ARGB pixels*/
class BitmapData {
public:
    UINT Width = 0, Height = 0, Stride = 0;
    void *Scan0 = nullptr;
};
class Image {
public:
    Image() : w(0), h(0), status(FileNotFound) {}
    virtual ~Image() { if (owned) free(px); }
    UINT GetWidth() const { return (UINT)w; }
    UINT GetHeight() const { return (UINT)h; }
    Status GetLastStatus() const { return status; }
    int w, h;
    uint32_t *px = nullptr;   /* 0xAARRGGBB */
    bool owned = true;
    Status status = Ok;
};
class Bitmap : public Image {
public:
    Bitmap() {}
    Bitmap(INT w, INT h, PixelFormatShim = PixelFormat32bppPARGB);   /* off-screen render target */
    explicit Bitmap(HDC, INT, INT, PixelFormatShim) { status = OutOfMemory; }
    static Bitmap *FromFile(const wchar_t *path);
    static Bitmap *FromStream(IStream *stream);
    static Bitmap *FromHBITMAP(HBITMAP, HPALETTE) { return nullptr; }
    Status LockBits(const Rect *r, ImageLockMode, PixelFormatShim, BitmapData *out);
    Status UnlockBits(BitmapData *out);
    uint32_t *TakePixels() { owned = false; return px; }   /* modbox takes ownership of the buffer */
};

/*startup (the whole shim is always available)*/
struct GdiplusStartupInput { UINT GdiplusVersion = 1; };
inline Status GdiplusStartup(ULONG_PTR *, const GdiplusStartupInput *, void *) { return Ok; }
inline void GdiplusShutdown(ULONG_PTR) {}

}  // namespace Gdiplus

#endif /*TF_GDIPLUS_SHIM_H*/
