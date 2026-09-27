#include "overlay.h"

#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "util.h"

using namespace Gdiplus;

namespace {

const wchar_t* kOverlayClass = L"OverlayMgrOverlayWnd";
const int kGripSize = 18;  // resize corner, in pixels
const int kMinSide = 16;
const double kPi = 3.14159265358979323846;

ULONG_PTR g_gdiplusToken = 0;
std::vector<MonitorInfo> g_monitors;

BOOL CALLBACK EnumMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM out) {
    MONITORINFO mi{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    MonitorInfo info;
    info.rect = mi.rcMonitor;
    info.width = mi.rcMonitor.right - mi.rcMonitor.left;
    info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;

    auto& list = *reinterpret_cast<std::vector<MonitorInfo>*>(out);
    // Primary first, so "monitor 0" in a config keeps meaning the main screen
    // even after displays are re-arranged.
    if (mi.dwFlags & MONITORINFOF_PRIMARY)
        list.insert(list.begin(), std::move(info));
    else
        list.push_back(std::move(info));
    return TRUE;
}

// Builds the 5x5 colour matrix carrying opacity, tint and grayscale, so all
// three cost a single GDI+ draw.
ColorMatrix BuildColorMatrix(const Layer& l) {
    ColorMatrix m = {{{1, 0, 0, 0, 0},
                      {0, 1, 0, 0, 0},
                      {0, 0, 1, 0, 0},
                      {0, 0, 0, 1, 0},
                      {0, 0, 0, 0, 1}}};

    if (l.grayscale) {
        // Rec. 601 luminance: every output channel gets the same gray.
        const REAL r = 0.299f, g = 0.587f, b = 0.114f;
        m.m[0][0] = r; m.m[0][1] = r; m.m[0][2] = r;
        m.m[1][0] = g; m.m[1][1] = g; m.m[1][2] = g;
        m.m[2][0] = b; m.m[2][1] = b; m.m[2][2] = b;
    }

    if (l.useTint) {
        // Scaling output columns tints whatever the previous stage produced,
        // which is why it composes correctly on top of grayscale.
        const REAL tr = l.tintR / 255.0f, tg = l.tintG / 255.0f, tb = l.tintB / 255.0f;
        for (int row = 0; row < 4; ++row) {
            m.m[row][0] *= tr;
            m.m[row][1] *= tg;
            m.m[row][2] *= tb;
        }
    }

    m.m[3][3] = std::max(0, std::min(255, l.opacity)) / 255.0f;
    return m;
}

Bitmap* AsBitmap(const std::shared_ptr<void>& p) {
    return static_cast<Bitmap*>(p.get());
}

// Finds the CLSID of an installed GDI+ encoder, e.g. for "image/png".
bool GetEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT count = 0, bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || bytes == 0) return false;
    std::vector<unsigned char> buf(bytes);
    auto* codecs = reinterpret_cast<ImageCodecInfo*>(buf.data());
    if (GetImageEncoders(count, bytes, codecs) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < len) {
        unsigned v = data[i] << 16;
        const bool two = (i + 1 < len);
        if (two) v |= data[i + 1] << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += two ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace

bool GdiPlusStart() {
    GdiplusStartupInput input;
    return GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Ok;
}

void GdiPlusStop() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

void RefreshMonitors() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc,
                        reinterpret_cast<LPARAM>(&g_monitors));
    if (g_monitors.empty()) {
        // Nothing reported: fall back to the primary metrics so layers still
        // land somewhere sensible instead of collapsing onto 0x0.
        MonitorInfo info;
        info.rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        info.width = info.rect.right;
        info.height = info.rect.bottom;
        g_monitors.push_back(info);
    }
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        wchar_t buf[64];
        swprintf(buf, 64, L"מסך %d  (%dx%d)", (int)i + 1, g_monitors[i].width,
                 g_monitors[i].height);
        g_monitors[i].label = buf;
    }
}

const std::vector<MonitorInfo>& Monitors() {
    if (g_monitors.empty()) RefreshMonitors();
    return g_monitors;
}

const MonitorInfo& MonitorAt(int index) {
    const auto& list = Monitors();
    if (index < 0 || index >= (int)list.size()) return list[0];
    return list[index];
}

// GDI+ knows PNG/JPEG/BMP/GIF/TIFF only; WebP (and HEIC, when its codec is
// installed) comes through WIC, which Windows 10 1809+ ships a WebP decoder for.
static Bitmap* DecodeWithWic(const std::wstring& path) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return nullptr;

    Bitmap* out = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
        auto* bmp = new Bitmap((INT)w, (INT)h, PixelFormat32bppARGB);
        BitmapData data{};
        Rect rc(0, 0, (INT)w, (INT)h);
        if (bmp->GetLastStatus() == Ok &&
            bmp->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok) {
            const HRESULT hr = conv->CopyPixels(nullptr, (UINT)data.Stride,
                                                (UINT)data.Stride * h,
                                                static_cast<BYTE*>(data.Scan0));
            bmp->UnlockBits(&data);
            if (SUCCEEDED(hr)) out = bmp;
        }
        if (!out) delete bmp;
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return out;
}

static std::unique_ptr<Bitmap> OpenImageFile(const std::wstring& path) {
    if (path.empty()) return nullptr;
    std::unique_ptr<Bitmap> bmp(Bitmap::FromFile(path.c_str(), FALSE));
    if (bmp && bmp->GetLastStatus() == Ok) return bmp;
    return std::unique_ptr<Bitmap>(DecodeWithWic(path));
}

static double ColorDistance(uint32_t a, uint32_t b) {
    const int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
    const int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
    const int db = (int)(a & 0xFF) - (int)(b & 0xFF);
    return std::sqrt((double)(dr * dr + dg * dg + db * db));
}

// Clears the flat background of a picture saved without transparency. The
// background colour is the one most of the four corners agree on; pixels of
// that colour are removed only where they connect to the image's edge, so a
// white letter inside a logo on a white square stays white. Pixels near the
// threshold fade out and have the background colour un-blended from them, so
// anti-aliased edges do not keep a pale halo.
static std::unique_ptr<Bitmap> RemoveBackground(Bitmap& src, int tolerance) {
    const int w = (int)src.GetWidth(), h = (int)src.GetHeight();
    if (w <= 0 || h <= 0) return nullptr;

    std::vector<uint32_t> px((size_t)w * h);
    {
        BitmapData data{};
        Rect rc(0, 0, w, h);
        if (src.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return nullptr;
        for (int y = 0; y < h; ++y)
            memcpy(&px[(size_t)y * w], static_cast<BYTE*>(data.Scan0) + (size_t)y * data.Stride,
                   (size_t)w * 4);
        src.UnlockBits(&data);
    }

    const uint32_t corners[4] = {px[0], px[(size_t)w - 1], px[(size_t)(h - 1) * w],
                                 px[(size_t)h * w - 1]};
    int best = 0, bestVotes = -1;
    for (int i = 0; i < 4; ++i) {
        int votes = 0;
        for (int j = 0; j < 4; ++j)
            if (ColorDistance(corners[i], corners[j]) < 30) ++votes;
        if (votes > bestVotes) { best = i; bestVotes = votes; }
    }
    const uint32_t key = corners[best];
    // Already transparent at the corners: there is no background to remove.
    if ((key >> 24) < 128) return nullptr;

    const double thresh = std::clamp(tolerance, 1, 100) * 2.55;
    const double inner = thresh * 0.5;

    // Flood fill from every edge pixel close enough to the key colour.
    std::vector<uint8_t> bg(px.size(), 0);
    std::vector<int> stack;
    auto seed = [&](int x, int y) {
        const size_t i = (size_t)y * w + x;
        if (!bg[i] && ColorDistance(px[i], key) <= thresh) {
            bg[i] = 1;
            stack.push_back((int)i);
        }
    };
    for (int x = 0; x < w; ++x) { seed(x, 0); seed(x, h - 1); }
    for (int y = 0; y < h; ++y) { seed(0, y); seed(w - 1, y); }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        if (x > 0) seed(x - 1, y);
        if (x + 1 < w) seed(x + 1, y);
        if (y > 0) seed(x, y - 1);
        if (y + 1 < h) seed(x, y + 1);
    }

    const double kr = (key >> 16) & 0xFF, kg = (key >> 8) & 0xFF, kb = key & 0xFF;
    for (size_t i = 0; i < px.size(); ++i) {
        if (!bg[i]) continue;
        const double d = ColorDistance(px[i], key);
        if (d <= inner) {
            px[i] = 0;
            continue;
        }
        const double a = (d - inner) / (thresh - inner);
        auto unblend = [&](uint32_t c, double k) {
            return (uint32_t)std::clamp((c - k * (1 - a)) / a, 0.0, 255.0);
        };
        const uint32_t r = unblend((px[i] >> 16) & 0xFF, kr);
        const uint32_t g = unblend((px[i] >> 8) & 0xFF, kg);
        const uint32_t b = unblend(px[i] & 0xFF, kb);
        const uint32_t al = (uint32_t)std::lround((px[i] >> 24) * a);
        px[i] = (al << 24) | (r << 16) | (g << 8) | b;
    }

    auto out = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
    BitmapData data{};
    Rect rc(0, 0, w, h);
    if (out->GetLastStatus() != Ok ||
        out->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok)
        return nullptr;
    for (int y = 0; y < h; ++y)
        memcpy(static_cast<BYTE*>(data.Scan0) + (size_t)y * data.Stride, &px[(size_t)y * w],
               (size_t)w * 4);
    out->UnlockBits(&data);
    return out;
}

// The layer's picture as it will be drawn: decoded, and with the background
// cleared when bgTolerance > 0.
static std::unique_ptr<Bitmap> OpenLayerImage(const std::wstring& path, int bgTolerance) {
    auto bmp = OpenImageFile(path);
    if (bmp && bgTolerance > 0)
        if (auto cleared = RemoveBackground(*bmp, bgTolerance)) return cleared;
    return bmp;
}

bool ImagePixelSize(const std::wstring& path, int& outW, int& outH) {
    if (path.empty()) return false;
    auto bmp = OpenImageFile(path);
    if (!bmp) return false;
    outW = (int)bmp->GetWidth();
    outH = (int)bmp->GetHeight();
    return outW > 0 && outH > 0;
}

std::string ImageToDataUri(const std::wstring& path, int maxSide, int bgTolerance) {
    if (path.empty()) return {};
    auto srcBmp = OpenLayerImage(path, bgTolerance);
    if (!srcBmp) return {};
    Bitmap& src = *srcBmp;

    const int sw = (int)src.GetWidth(), sh = (int)src.GetHeight();
    if (sw <= 0 || sh <= 0) return {};

    // Downscale for the preview: a 4000px photo would otherwise become a
    // multi-megabyte data URI on every selection change.
    double scale = 1.0;
    const int longest = std::max(sw, sh);
    if (maxSide > 0 && longest > maxSide) scale = (double)maxSide / longest;
    const int dw = std::max(1, (int)std::lround(sw * scale));
    const int dh = std::max(1, (int)std::lround(sh * scale));

    Bitmap dst(dw, dh, PixelFormat32bppARGB);
    {
        Graphics g(&dst);
        g.Clear(Color(0, 0, 0, 0));
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.DrawImage(&src, Rect(0, 0, dw, dh), 0, 0, sw, sh, UnitPixel);
    }

    CLSID png;
    if (!GetEncoderClsid(L"image/png", &png)) return {};

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return {};

    std::string uri;
    if (dst.Save(stream, &png, nullptr) == Ok) {
        HGLOBAL mem = nullptr;
        if (GetHGlobalFromStream(stream, &mem) == S_OK) {
            const SIZE_T size = GlobalSize(mem);
            auto* bytes = static_cast<unsigned char*>(GlobalLock(mem));
            if (bytes && size) {
                uri = "data:image/png;base64," + Base64(bytes, size);
                GlobalUnlock(mem);
            }
        }
    }
    stream->Release();
    return uri;
}

// --- OverlayWindow ---------------------------------------------------------

void OverlayWindow::RegisterWindowClass(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &OverlayWindow::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    RegisterClassW(&wc);
}

OverlayWindow::OverlayWindow(HINSTANCE inst, int index, const Layer& layer,
                             GeometryFn onGeometry)
    : inst_(inst), index_(index), layer_(layer), onGeometry_(std::move(onGeometry)) {
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            kOverlayClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) return;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    LoadLayerImage();
    Render();
    ApplyVisibility();
}

OverlayWindow::~OverlayWindow() {
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void OverlayWindow::Update(const Layer& layer) {
    const bool imageChanged = layer.image != loadedPath_ || BgKey(layer) != loadedBg_;
    const bool clickThroughChanged = layer.clickThrough != layer_.clickThrough;
    layer_ = layer;
    if (imageChanged) LoadLayerImage();
    if (clickThroughChanged && !positionMode_) ApplyClickThrough();
    Render();
    ApplyVisibility();
}

void OverlayWindow::SetHiddenByHotkey(bool hidden) {
    hiddenByHotkey_ = hidden;
    ApplyVisibility();
}

void OverlayWindow::SetPositionMode(bool on) {
    if (positionMode_ == on) return;
    positionMode_ = on;
    grab_ = Grab::None;
    ApplyClickThrough();
    Render();
    ApplyVisibility();
}

void OverlayWindow::ApplyClickThrough() {
    if (!hwnd_) return;
    // Position mode must receive clicks, so WS_EX_TRANSPARENT comes off for as
    // long as it lasts and goes back afterwards.
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (positionMode_ || !layer_.clickThrough)
        ex &= ~WS_EX_TRANSPARENT;
    else
        ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

void OverlayWindow::Reposition() { Render(); }

void OverlayWindow::LoadLayerImage() {
    loadedPath_ = layer_.image;
    loadedBg_ = BgKey(layer_);
    bitmap_.reset();
    imageW_ = imageH_ = 0;
    if (loadedPath_.empty()) return;

    auto* bmp = OpenLayerImage(loadedPath_, loadedBg_).release();
    if (!bmp) return;
    imageW_ = (int)bmp->GetWidth();
    imageH_ = (int)bmp->GetHeight();
    bitmap_ = std::shared_ptr<void>(bmp, [](void* p) { delete static_cast<Bitmap*>(p); });
}

SIZE OverlayWindow::RotatedExtent() const {
    const int w = std::max(kMinSide, layer_.width);
    const int h = std::max(kMinSide, layer_.height);
    if (std::fabs(layer_.rotation) < 0.01) return SIZE{w, h};

    // Bounding box of the rotated rectangle, so the window is always big enough
    // and the corners are not clipped off.
    const double rad = layer_.rotation * kPi / 180.0;
    const double c = std::fabs(std::cos(rad)), s = std::fabs(std::sin(rad));
    return SIZE{(int)std::ceil(w * c + h * s), (int)std::ceil(w * s + h * c)};
}

RECT OverlayWindow::ComputeRect() const {
    const MonitorInfo& mon = MonitorAt(layer_.monitor);
    const SIZE extent = RotatedExtent();

    const int dx = (int)std::lround(mon.width * layer_.offsetX / 100.0);
    const int dy = (int)std::lround(mon.height * layer_.offsetY / 100.0);

    int x;
    switch (layer_.anchorH) {
        case ANCHOR_LEFT:  x = mon.rect.left + dx; break;
        case ANCHOR_RIGHT: x = mon.rect.right - extent.cx - dx; break;
        default:           x = mon.rect.left + (mon.width - extent.cx) / 2 + dx; break;
    }
    int y;
    switch (layer_.anchorV) {
        case ANCHOR_TOP:    y = mon.rect.top + dy; break;
        case ANCHOR_BOTTOM: y = mon.rect.bottom - extent.cy - dy; break;
        default:            y = mon.rect.top + (mon.height - extent.cy) / 2 + dy; break;
    }
    return RECT{x, y, x + extent.cx, y + extent.cy};
}

void OverlayWindow::Render() {
    if (!hwnd_) return;

    const RECT r = ComputeRect();
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        ReleaseDC(nullptr, screen);
        return;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    {
        Graphics g(mem);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.Clear(Color(0, 0, 0, 0));

        if (positionMode_) {
            // A faint scrim makes the whole layer grabbable: a layered window
            // only hit-tests where alpha is non-zero, so a mostly transparent
            // watermark would otherwise be almost impossible to click.
            SolidBrush scrim(Color(40, 56, 189, 248));
            g.FillRectangle(&scrim, 0, 0, w, h);
        }

        const int lw = std::max(kMinSide, layer_.width);
        const int lh = std::max(kMinSide, layer_.height);

        if (bitmap_ && imageW_ > 0 && imageH_ > 0) {
            ImageAttributes attrs;
            ColorMatrix cm = BuildColorMatrix(layer_);
            attrs.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

            GraphicsState saved = g.Save();
            if (std::fabs(layer_.rotation) >= 0.01) {
                g.TranslateTransform(w / 2.0f, h / 2.0f);
                g.RotateTransform((REAL)layer_.rotation);
                g.TranslateTransform(-lw / 2.0f, -lh / 2.0f);
            } else {
                g.TranslateTransform((REAL)((w - lw) / 2), (REAL)((h - lh) / 2));
            }
            g.DrawImage(AsBitmap(bitmap_), Rect(0, 0, lw, lh), 0, 0, imageW_,
                        imageH_, UnitPixel, &attrs);
            g.Restore(saved);
        } else if (positionMode_) {
            // No image loaded yet, but there still has to be something to drag.
            SolidBrush fill(Color(90, 148, 163, 184));
            g.FillRectangle(&fill, 0, 0, w, h);
        }

        if (positionMode_) {
            Pen pen(Color(230, 56, 189, 248), 2.0f);
            pen.SetDashStyle(DashStyleDash);
            g.DrawRectangle(&pen, 1, 1, w - 3, h - 3);

            SolidBrush grip(Color(230, 56, 189, 248));
            g.FillRectangle(&grip, w - kGripSize, h - kGripSize, kGripSize - 1,
                            kGripSize - 1);
        }
    }

    // UpdateLayeredWindow expects premultiplied alpha; GDI+ produced straight
    // alpha, so premultiply the bits before handing them over.
    auto* px = static_cast<unsigned char*>(bits);
    for (int i = 0; i < w * h; ++i) {
        const unsigned a = px[3];
        if (a != 255) {
            px[0] = (unsigned char)(px[0] * a / 255);
            px[1] = (unsigned char)(px[1] * a / 255);
            px[2] = (unsigned char)(px[2] * a / 255);
        }
        px += 4;
    }

    POINT dst{r.left, r.top};
    POINT src{0, 0};
    SIZE size{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

void OverlayWindow::ApplyVisibility() {
    if (!hwnd_) return;
    // A layer whose file failed to load draws nothing, so keep its window out
    // of the way entirely — unless it is being positioned, where the scrim is
    // the only thing the user has to grab.
    const bool hasContent = bitmap_ != nullptr;
    const bool visible =
        layer_.enabled && !hiddenByHotkey_ && (hasContent || positionMode_);
    // SWP_NOACTIVATE leaves focus with whatever the user is actually working in.
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT OverlayWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (!positionMode_) break;
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            GetWindowRect(hwnd_, &grabRect_);
            const int w = grabRect_.right - grabRect_.left;
            const int h = grabRect_.bottom - grabRect_.top;
            grab_ = (pt.x >= w - kGripSize && pt.y >= h - kGripSize) ? Grab::Resize
                                                                    : Grab::Move;
            GetCursorPos(&grabOrigin_);
            SetCapture(hwnd_);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!positionMode_) break;

            if (grab_ == Grab::None) {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                RECT wr;
                GetWindowRect(hwnd_, &wr);
                const bool onGrip = pt.x >= (wr.right - wr.left) - kGripSize &&
                                    pt.y >= (wr.bottom - wr.top) - kGripSize;
                SetCursor(LoadCursorW(nullptr, onGrip ? IDC_SIZENWSE : IDC_SIZEALL));
                return 0;
            }

            POINT now;
            GetCursorPos(&now);
            const int dx = now.x - grabOrigin_.x;
            const int dy = now.y - grabOrigin_.y;
            const MonitorInfo& mon = MonitorAt(layer_.monitor);

            if (grab_ == Grab::Move) {
                // Convert the new top-left back into anchor-relative percentages,
                // which is what the config actually stores.
                const int w = grabRect_.right - grabRect_.left;
                const int h = grabRect_.bottom - grabRect_.top;
                const int nx = grabRect_.left + dx;
                const int ny = grabRect_.top + dy;

                double ox;
                switch (layer_.anchorH) {
                    case ANCHOR_LEFT:  ox = nx - mon.rect.left; break;
                    case ANCHOR_RIGHT: ox = mon.rect.right - (nx + w); break;
                    default:           ox = nx - (mon.rect.left + (mon.width - w) / 2); break;
                }
                double oy;
                switch (layer_.anchorV) {
                    case ANCHOR_TOP:    oy = ny - mon.rect.top; break;
                    case ANCHOR_BOTTOM: oy = mon.rect.bottom - (ny + h); break;
                    default:            oy = ny - (mon.rect.top + (mon.height - h) / 2); break;
                }
                layer_.offsetX = mon.width ? ox * 100.0 / mon.width : 0;
                layer_.offsetY = mon.height ? oy * 100.0 / mon.height : 0;
            } else {
                // RECT members are LONG; narrow explicitly so std::max has one
                // type to work with.
                int nw = std::max(kMinSide, (int)(grabRect_.right - grabRect_.left) + dx);
                int nh = std::max(kMinSide, (int)(grabRect_.bottom - grabRect_.top) + dy);
                if (layer_.lockAspect && layer_.width > 0 && layer_.height > 0) {
                    const double ratio = (double)layer_.height / layer_.width;
                    nh = std::max(kMinSide, (int)std::lround(nw * ratio));
                }
                layer_.width = nw;
                layer_.height = nh;
            }

            Render();
            if (onGeometry_)
                onGeometry_(index_, Geometry{layer_.offsetX, layer_.offsetY,
                                             layer_.width, layer_.height});
            return 0;
        }

        case WM_LBUTTONUP:
            if (grab_ != Grab::None) {
                grab_ = Grab::None;
                ReleaseCapture();
            }
            return 0;

        case WM_SETCURSOR:
            if (positionMode_) return TRUE;
            break;

        case WM_NCHITTEST:
            return positionMode_ ? HTCLIENT : HTTRANSPARENT;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
