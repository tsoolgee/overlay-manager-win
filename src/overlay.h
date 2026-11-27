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

// What a timer layer (timer.h) needs from the app: its state changed (save
// for a deliberate change, not for every tick), and the menu's app-wide items.
struct TimerHooks {
    std::function<void(int index, const Layer& layer, bool save)> changed;
    std::function<bool()> autoStart;
    std::function<void()> toggleAutoStart;
    std::function<void(int index)> openManager;
};

class OverlayWindow {
public:
    using GeometryFn = std::function<void(int index, const Geometry&)>;

    static void RegisterWindowClass(HINSTANCE inst);

    OverlayWindow(HINSTANCE inst, int index, const Layer& layer, GeometryFn onGeometry,
                  TimerHooks timerHooks = {});
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Pushes new layer data in. Reloads the bitmap only when the path changed,
    // so dragging the opacity slider does not hit the disk on every frame.
    void Update(const Layer& layer);

    void SetIndex(int index) { index_ = index; }
    HWND Hwnd() const { return hwnd_; }
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
    RECT AnchoredRect(SIZE extent) const;
    SIZE RotatedExtent() const;

    // The kiosk timer, when the layer is one (timer.h).
    LRESULT HandleTimer(UINT msg, WPARAM wp, LPARAM lp, bool& handled);
    void TimerTick();
    void TimerMenu();
    void TimerChanged(bool save);
    double TimerScale() const;

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
    SIZE grabSize_{};     // the layer's own size when the grab began
    SIZE grabExtent_{};   // its window (the rotated bounding box) at that moment

    TimerHooks timerHooks_;
    bool timer_ = false;
    bool expanded_ = false;      // hovered: the full panel instead of the bar
    int hoverPart_ = 0;          // timerface::Part under the mouse
    bool tracking_ = false;      // TrackMouseEvent armed for WM_MOUSELEAVE
    bool pressing_ = false, dragged_ = false;
    int pressPart_ = 0;
    POINT pressCursor_{};
    RECT pressRect_{};
    unsigned long long lastTick_ = 0;
    long long shownRemain_ = -1, shownUsed_ = -1;
    double reportedRemain_ = -1, reportedUsed_ = -1;  // what the app last heard
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

// Pictures built into the program, offered next to choosing a file. A layer
// that uses one stores "sticker:<name>" as its image, so it keeps working when
// no copy of the picture exists on the disk.
enum class StickerGroup { All, Main, More };
std::vector<std::wstring> StickerNames(StickerGroup group = StickerGroup::All);
bool IsStickerPath(const std::wstring& image);
std::wstring StickerPath(const std::wstring& name);
// The width a layer gets when this sticker is picked; 0 if it is not a sticker.
int StickerDefaultWidth(const std::wstring& image);
// Its usual place (anchored bottom-left), in percent; false if not a sticker.
bool StickerDefaultPlace(const std::wstring& image, double& offX, double& offY);
