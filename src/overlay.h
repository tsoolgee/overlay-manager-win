// overlay.h — one always-on-top, per-pixel-alpha window per visible layer.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model.h"

struct MonitorInfo {
    RECT rect{};         // virtual-screen coordinates, physical pixels
    int width = 0;
    int height = 0;
    std::wstring label;  // "מסך 1  (1920x1080)"
};

// Enumerates displays into a stable, primary-first order. Called again on
// WM_DISPLAYCHANGE: monitors get plugged and unplugged, and stale rectangles
// would otherwise strand overlays off-screen.
void RefreshMonitors();
const std::vector<MonitorInfo>& Monitors();
const MonitorInfo& MonitorAt(int index);

// Reported when the user drags or resizes an overlay directly on screen, so the
// manager UI and the config can follow along.
struct Geometry {
    double offsetX;
    double offsetY;
    int width;
    int height;
};

class OverlayWindow {
public:
    using GeometryFn = std::function<void(int index, const Geometry&)>;

    static void RegisterWindowClass(HINSTANCE inst);

    OverlayWindow(HINSTANCE inst, int index, const Layer& layer, GeometryFn onGeometry);
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Pushes new layer data in. Reloads the bitmap only when the path changed,
    // so dragging the opacity slider does not hit the disk on every frame.
    void Update(const Layer& layer);

    void SetIndex(int index) { index_ = index; }
    void SetHiddenByHotkey(bool hidden);
    void SetPositionMode(bool on);
    bool PositionMode() const { return positionMode_; }

    void Reposition();  // re-run the anchor maths, e.g. after a display change

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // Not named LoadImage: <windows.h> defines that as a macro for LoadImageW.
    void LoadLayerImage();
    void Render();
    void ApplyVisibility();
    void ApplyClickThrough();
    RECT ComputeRect() const;
    SIZE RotatedExtent() const;

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    int index_ = 0;
    Layer layer_;
    GeometryFn onGeometry_;

    std::wstring loadedPath_;
    int loadedBg_ = 0;           // BgKey the bitmap was produced with
    // Gdiplus::Bitmap, type-erased so <gdiplus.h> stays out of this header.
    std::shared_ptr<void> bitmap_;
    int imageW_ = 0, imageH_ = 0;

    bool hiddenByHotkey_ = false;
    bool positionMode_ = false;

    // Live drag/resize state.
    enum class Grab { None, Move, Resize };
    Grab grab_ = Grab::None;
    POINT grabOrigin_{};
    RECT grabRect_{};
};

// Starts and stops GDI+ for the process.
bool GdiPlusStart();
void GdiPlusStop();

// Reads an image file and returns "data:image/png;base64,...", scaled so the
// longest side is at most `maxSide`. Feeds the preview in the manager UI.
// With bgTolerance > 0 the preview shows the background already removed.
std::string ImageToDataUri(const std::wstring& path, int maxSide, int bgTolerance = 0);

// The background-removal tolerance a layer's picture is drawn with; 0 = off.
inline int BgKey(const Layer& l) { return l.removeBg ? l.bgTolerance : 0; }

// Native pixel size of an image file, for the aspect-ratio lock.
bool ImagePixelSize(const std::wstring& path, int& outW, int& outH);
