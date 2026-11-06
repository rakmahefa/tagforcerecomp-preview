/* Leitura/gravacao de PNG e JPEG.
 * Windows: WIC (nativo). Linux: libpng / libjpeg-turbo (a camada de compatibilidade, runtime/plat/,
 * cuida do resto do Win32; aqui o formato pede bibliotecas de verdade). */
#include "imgio.h"
#include "util.h"
#include "windows.h"
#include <string>
#include <stdio.h>
#include <vector>

#ifdef __linux__
#include <png.h>
#include <jpeglib.h>

bool img_load(const char *path, int *w, int *h, std::vector<uint32_t> &rgba) {
    std::wstring wp = to_wide(path);
    FILE *f = _wfopen(wp.c_str(), L"rb");
    if (!f) return false;
    unsigned char sig[8];
    bool is_png = fread(sig, 1, 8, f) == 8 && png_sig_cmp(sig, 0, 8) == 0;
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (is_png) {
        png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        png_infop info = p ? png_create_info_struct(p) : nullptr;
        if (p && info && !setjmp(png_jmpbuf(p))) {
            png_init_io(p, f);
            png_read_info(p, info);
            unsigned long wq = png_get_image_width(p, info), hq = png_get_image_height(p, info);
            if (wq && hq && wq <= 16384 && hq <= 16384) {
                png_set_strip_16(p);
                png_set_palette_to_rgb(p);
                png_set_tRNS_to_alpha(p);
                int ct = png_get_color_type(p, info);
                if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_GRAY) png_set_filler(p, 0xff, PNG_FILLER_AFTER);
                if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(p);
                png_read_update_info(p, info);
                size_t rowb = png_get_rowbytes(p, info);
                std::vector<unsigned char> rows(rowb * hq);
                std::vector<png_bytep> ptrs(hq);
                for (unsigned long y = 0; y < hq; y++) ptrs[y] = rows.data() + y * rowb;
                png_read_image(p, ptrs.data());
                rgba.resize((size_t)wq * hq);
                bool have_alpha = png_get_color_type(p, info) == PNG_COLOR_TYPE_RGBA;
                for (unsigned long y = 0; y < hq; y++) {
                    const unsigned char *r = rows.data() + y * rowb;
                    for (unsigned long x = 0; x < wq; x++) {
                        const unsigned char *s = r + x * 4;
                        rgba[(size_t)y * wq + x] = have_alpha
                            ? ((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]
                            : 0xFF000000u | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
                    }
                }
                *w = (int)wq; *h = (int)hq;
                ok = true;
            }
        }
        if (p) png_destroy_read_struct(&p, info ? &info : nullptr, nullptr);
    } else {   /* JPEG: setjmp error exit (a broken file skips the texture instead of dying) */
        jpeg_decompress_struct cinfo{};
        jpeg_error_mgr jerr{};
        cinfo.err = jpeg_std_error(&jerr);
        static jmp_buf jb_ok, jb_unused;   /* thread-safety by try-order: the decoders run one at a time per thread */
        struct JErr { jpeg_error_mgr base; jmp_buf jb; } jerrb;
        cinfo.err = jpeg_std_error((jpeg_error_mgr *)&jerrb);
        jerrb.base.error_exit = [](j_common_ptr ci) { JErr *e = (JErr *)ci->err; longjmp(e->jb, 1); };
        (void)jerr; (void)jb_ok; (void)jb_unused;
        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, f);
        if (setjmp(jerrb.jb) == 0 && jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK && cinfo.image_width <= 16384 && cinfo.image_height <= 16384) {
            cinfo.out_color_space = JCS_RGB;
            jpeg_start_decompress(&cinfo);
            int wq = (int)cinfo.output_width, hq = (int)cinfo.output_height;
            rgba.resize((size_t)wq * hq);
            std::vector<unsigned char> row((size_t)wq * 3);
            for (int y = 0; y < hq && cinfo.output_scanline < (unsigned)hq; y++) {
                unsigned char *rp = row.data();
                jpeg_read_scanlines(&cinfo, &rp, 1);
                for (int x = 0; x < wq; x++) rgba[(size_t)y * wq + x] = 0xFF000000u | ((uint32_t)row[x * 3] << 16) | ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3 + 2];
            }
            *w = wq; *h = hq;
            ok = true;
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
    }
    fclose(f);
    return ok;
}

bool img_save_png(const char *path, int w, int h, const uint32_t *rgba) {
    std::wstring wp = to_wide(path);
    FILE *f = _wfopen(wp.c_str(), L"wb");
    if (!f) return false;
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = p ? png_create_info_struct(p) : nullptr;
    bool ok = false;
    if (p && info && !setjmp(png_jmpbuf(p))) {
        png_init_io(p, f);
        png_set_IHDR(p, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(p, info);
        std::vector<unsigned char> row((size_t)w * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t px = rgba[(size_t)y * w + x];
                row[x * 4] = (unsigned char)((px >> 16) & 255);
                row[x * 4 + 1] = (unsigned char)((px >> 8) & 255);
                row[x * 4 + 2] = (unsigned char)(px & 255);
                row[x * 4 + 3] = (unsigned char)((px >> 24) & 255);
            }
            png_write_row(p, row.data());
        }
        png_write_end(p, info);
        ok = true;
    }
    if (p) png_destroy_write_struct(&p, info ? &info : nullptr);
    fclose(f);
    return ok;
}

bool img_save_jpg(const char *path, int w, int h, const uint32_t *rgba) {
    std::wstring wp = to_wide(path);
    FILE *f = _wfopen(wp.c_str(), L"wb");
    if (!f) return false;
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = w; cinfo.image_height = h;
    cinfo.input_components = 3; cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    std::vector<unsigned char> row((size_t)w * 3);
    while (cinfo.next_scanline < (unsigned)h) {
        for (int x = 0; x < w; x++) {
            uint32_t px = rgba[(size_t)cinfo.next_scanline * w + x];
            row[x * 3] = (unsigned char)((px >> 16) & 255);
            row[x * 3 + 1] = (unsigned char)((px >> 8) & 255);
            row[x * 3 + 2] = (unsigned char)(px & 255);
        }
        unsigned char *rp = row.data();
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    return true;
}

#else /* _WIN32: the original WIC implementation */
/* Leitura/gravacao de PNG pelo WIC (Windows Imaging Component, nativo do Windows): texturas substitutas e despejo de texturas. */
#include "imgio.h"
#include "util.h"
#include <windows.h>
#include <wincodec.h>
#include <string>
#include <stdio.h>

static IWICImagingFactory *factory() {
    static thread_local IWICImagingFactory *f; static thread_local bool tried;   /* uma fabrica por thread (leitura em segundo plano + despejo) */
    if (!tried) {
        tried = true;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)hr;   /* S_FALSE/RPC_E_CHANGED_MODE: a thread ja tem COM, serve */
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) f = nullptr;
    }
    return f;
}
bool img_load(const char *path, int *w, int *h, std::vector<uint32_t> &rgba) {
    IWICImagingFactory *f = factory(); if (!f) return false;
    std::wstring wp = to_wide(path);
    IWICBitmapDecoder *dec = nullptr; IWICBitmapFrameDecode *fr = nullptr; IWICFormatConverter *cv = nullptr; bool ok = false;
    if (SUCCEEDED(f->CreateDecoderFromFilename(wp.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
        UINT ww = 0, hh = 0; fr->GetSize(&ww, &hh);
        if (ww && hh && ww <= 16384 && hh <= 16384 && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            rgba.resize((size_t)ww * hh);
            if (SUCCEEDED(cv->CopyPixels(nullptr, ww * 4, (UINT)(rgba.size() * 4), (BYTE *)rgba.data()))) { *w = (int)ww; *h = (int)hh; ok = true; }
        }
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ok;
}
bool img_save_png(const char *path, int w, int h, const uint32_t *rgba) {
    IWICImagingFactory *f = factory(); if (!f) return false;
    std::wstring wp = to_wide(path);
    IWICStream *st = nullptr; IWICBitmapEncoder *enc = nullptr; IWICBitmapFrameEncode *fr = nullptr; IPropertyBag2 *pb = nullptr; bool ok = false;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) &&
        SUCCEEDED(fr->SetSize((UINT)w, (UINT)h))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;   /* o codificador PNG aceita BGRA */
        std::vector<uint32_t> bgra((size_t)w * h);
        for (size_t i = 0; i < bgra.size(); i++) { uint32_t p = rgba[i]; bgra[i] = (p & 0xff00ff00u) | ((p & 0xff) << 16) | ((p >> 16) & 0xff); }
        if (SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat32bppBGRA && SUCCEEDED(fr->WritePixels((UINT)h, (UINT)w * 4, (UINT)((size_t)w * h * 4), (BYTE *)bgra.data())) &&
            SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) ok = true;
    }
    if (pb) pb->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release();
    return ok;
}

bool img_save_jpg(const char *path, int w, int h, const uint32_t *rgba) {
    IWICImagingFactory *f = factory(); if (!f) return false;
    std::wstring wp = to_wide(path);
    IWICStream *st = nullptr; IWICBitmapEncoder *enc = nullptr; IWICBitmapFrameEncode *fr = nullptr; IPropertyBag2 *pb = nullptr; bool ok = false;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &pb))) {
        PROPBAG2 opt = {}; opt.pstrName = (LPOLESTR)L"ImageQuality"; VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.9f; pb->Write(1, &opt, &v);
        if (SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize((UINT)w, (UINT)h))) {
            WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
            std::vector<uint8_t> bgr((size_t)w * h * 3);
            for (size_t i = 0; i < (size_t)w * h; i++) { const uint32_t p = rgba[i]; bgr[i * 3] = (uint8_t)(p >> 16); bgr[i * 3 + 1] = (uint8_t)(p >> 8); bgr[i * 3 + 2] = (uint8_t)p; }
            if (SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat24bppBGR && SUCCEEDED(fr->WritePixels((UINT)h, (UINT)w * 3, (UINT)bgr.size(), bgr.data())) &&
                SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) ok = true;
        }
    }
    if (pb) pb->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release();
    return ok;
}
#endif
