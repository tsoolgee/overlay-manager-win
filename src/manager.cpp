#include "manager.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <vector>

#include "model.h"
#include "resource.h"
#include "util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* kManagerClass = L"OverlayMgrManagerWnd";
const wchar_t* kTitle = L"Overlay Manager - ניהול חותמות צפות";

// The page ships inside the executable as an RCDATA resource, so there is a
// single file to distribute and nothing to go missing next to it.
std::wstring LoadUiHtml() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_UI_HTML), RT_RCDATA);
    if (!res) return L"<h1>UI resource missing</h1>";
    HGLOBAL data = LoadResource(nullptr, res);
    if (!data) return L"<h1>UI resource missing</h1>";
    const DWORD size = SizeofResource(nullptr, res);
    const char* bytes = static_cast<const char*>(LockResource(data));
    if (!bytes || !size) return L"<h1>UI resource missing</h1>";

    // The .rc stores the file verbatim, so skip a UTF-8 BOM if the editor left one.
    size_t offset = 0;
    if (size >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF)
        offset = 3;
    return ToWide(std::string(bytes + offset, size - offset));
}

// The WebView keeps its cache beside the config, so uninstalling means
// deleting one folder.
std::wstring UserDataFolder() { return ConfigDir() + L"\\WebView2"; }

// The page's --bg for each theme: the frame is painted with it before the page
// draws, so neither a resize nor the first frame flashes a different colour.
COLORREF ThemeBackground(bool light) { return light ? RGB(0xee, 0xf2, 0xf7) : RGB(0x0b, 0x12, 0x20); }

// The process is per-monitor DPI aware, so window sizes are physical pixels;
// the page lays itself out in CSS pixels. Scaling by the DPI keeps the window
// the size the layout was designed for on a 125% or 150% display, where an
// unscaled 1080px window is only 864 or 720 CSS pixels wide. Both functions
// arrived in Windows 10 1607; before that the system DPI is the answer.
UINT DpiFor(HWND hwnd) {
    using ForWindow = UINT(WINAPI*)(HWND);
    using ForSystem = UINT(WINAPI*)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (hwnd) {
        if (auto f = reinterpret_cast<ForWindow>(GetProcAddress(user32, "GetDpiForWindow")))
            if (UINT d = f(hwnd)) return d;
    }
    if (auto f = reinterpret_cast<ForSystem>(GetProcAddress(user32, "GetDpiForSystem")))
        if (UINT d = f()) return d;
    HDC dc = GetDC(nullptr);
    const int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d > 0 ? (UINT)d : 96;
}

int Scaled(int cssPixels, UINT dpi) { return MulDiv(cssPixels, (int)dpi, 96); }

}  // namespace

struct ManagerWindow::Impl {
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool ready = false;
    std::vector<std::string> queue;
};

bool WebView2Available() {
    LPWSTR version = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool ok = SUCCEEDED(hr) && version != nullptr;
    if (version) CoTaskMemFree(version);
    return ok;
}

void ManagerWindow::RegisterWindowClass(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &ManagerWindow::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND paints the current theme's colour
    wc.lpszClassName = kManagerClass;
    RegisterClassW(&wc);
}

ManagerWindow::ManagerWindow(HINSTANCE inst, MessageFn onMessage, CloseFn onClose)
    : impl_(new Impl()), inst_(inst), onMessage_(std::move(onMessage)),
      onClose_(std::move(onClose)) {}

ManagerWindow::~ManagerWindow() {
    if (impl_) {
        if (impl_->controller) impl_->controller->Close();
        delete impl_;
        impl_ = nullptr;
    }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool ManagerWindow::Create(bool lightTheme) {
    if (!WebView2Available()) {
        const int answer = MessageBoxW(
            nullptr,
            L"הממשק של Overlay Manager מבוסס על רכיב WebView2 של מיקרוסופט, "
            L"והוא לא מותקן במחשב הזה.\n\n"
            L"רוב מחשבי Windows 11 וגרסאות עדכניות של Windows 10 כוללים אותו כברירת מחדל.\n\n"
            L"לפתוח את דף ההורדה של מיקרוסופט?",
            kTitle, MB_ICONWARNING | MB_YESNO | MB_RTLREADING | MB_RIGHT);
        if (answer == IDYES)
            ShellExecuteW(nullptr, L"open",
                          L"https://developer.microsoft.com/microsoft-edge/webview2/",
                          nullptr, nullptr, SW_SHOWNORMAL);
        return false;
    }

    // Size against the work area so the window fits on modest laptop screens
    // instead of opening larger than the desktop, and in CSS pixels (see
    // DpiFor) so a scaled display gets the same layout as an unscaled one.
    RECT work{0, 0, 1280, 800};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    const UINT dpi = DpiFor(nullptr);
    const int w = std::min(workW, std::min(Scaled(1240, dpi), std::max(Scaled(900, dpi), workW * 85 / 100)));
    const int h = std::min(workH, std::min(Scaled(820, dpi), std::max(Scaled(600, dpi), workH * 85 / 100)));

    hwnd_ = CreateWindowExW(
        0, kManagerClass, kTitle, WS_OVERLAPPEDWINDOW,
        work.left + (workW - w) / 2, work.top + (workH - h) / 2, w, h,
        nullptr, nullptr, inst_, this);
    if (!hwnd_) return false;

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowTextW(hwnd_, kTitle);
    SetTheme(lightTheme);

    const std::wstring html = LoadUiHtml();
    const bool light = lightTheme;

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, UserDataFolder().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, html, light](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) return hr;
                env->CreateCoreWebView2Controller(
                    hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, html, light](HRESULT hr2,
                                            ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(hr2) || !ctrl) return hr2;
                            impl_->controller = ctrl;
                            ctrl->get_CoreWebView2(&impl_->webview);
                            if (!impl_->webview) return E_FAIL;
                            ApplyWebViewBackground();

                            // This is an application UI, not a browser: turn off
                            // the context menu, dev tools and the zoom gesture so
                            // it cannot be knocked out of shape by accident.
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(impl_->webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);

                                // Suppressing the browser shortcuts (F5, Ctrl+P,
                                // and friends) arrived in Settings3; on an older
                                // runtime the app simply keeps them.
                                ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings.As(&settings3)) && settings3)
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }

                            // Stamp the theme before first paint, so opening in
                            // light mode does not flash the dark palette.
                            const std::wstring boot =
                                light ? L"document.documentElement.setAttribute('data-theme','light');"
                                      : L"document.documentElement.setAttribute('data-theme','dark');";
                            impl_->webview->AddScriptToExecuteOnDocumentCreated(
                                boot.c_str(), nullptr);

                            EventRegistrationToken token;
                            impl_->webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2WebMessageReceivedEventArgs* args)
                                        -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                                            if (onMessage_) onMessage_(ToUtf8(raw));
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token);

                            impl_->webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs*)
                                        -> HRESULT {
                                        impl_->ready = true;
                                        FlushQueue();
                                        return S_OK;
                                    }).Get(),
                                &token);

                            ResizeWebView();
                            impl_->webview->NavigateToString(html.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    return true;
}

void ManagerWindow::ResizeWebView() {
    if (!impl_ || !impl_->controller || !hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    impl_->controller->put_Bounds(rc);
}

void ManagerWindow::Post(const std::string& json) {
    if (!impl_) return;
    // Before the first navigation completes there is nothing to receive the
    // message, so hold it rather than dropping it.
    if (!impl_->ready || !impl_->webview) {
        impl_->queue.push_back(json);
        return;
    }
    impl_->webview->PostWebMessageAsJson(ToWide(json).c_str());
}

void ManagerWindow::FlushQueue() {
    if (!impl_ || !impl_->webview) return;
    for (const auto& msg : impl_->queue)
        impl_->webview->PostWebMessageAsJson(ToWide(msg).c_str());
    impl_->queue.clear();
}

void ManagerWindow::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);

    // The very first ShowWindow in a process ignores its argument and uses
    // STARTUPINFO.wShowWindow instead, so a launcher that started us hidden
    // would swallow the window on startup. SetWindowPos is not subject to that
    // rule, so it is the reliable way to force the window out.
    if (!IsWindowVisible(hwnd_))
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);

    SetForegroundWindow(hwnd_);
    if (impl_ && impl_->controller) impl_->controller->put_IsVisible(TRUE);
}

void ManagerWindow::Hide() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    // Letting the controller go invisible lets the WebView release its
    // compositor resources while the app sits in the tray.
    if (impl_ && impl_->controller) impl_->controller->put_IsVisible(FALSE);
}

bool ManagerWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void ManagerWindow::SetTheme(bool light) {
    light_ = light;
    if (!hwnd_) return;
    // A dark title bar over the dark page, instead of a white strip above it.
    // Attribute 20 is DWMWA_USE_IMMERSIVE_DARK_MODE from Windows 10 20H1 on;
    // 19 is the same switch in 1809-1909. Older systems ignore both.
    const BOOL dark = light ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
    ApplyWebViewBackground();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void ManagerWindow::ApplyWebViewBackground() {
    if (!impl_ || !impl_->controller) return;
    // What the WebView shows before the page paints, and while it resizes.
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(impl_->controller.As(&c2)) && c2) {
        const COLORREF bg = ThemeBackground(light_);
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, GetRValue(bg), GetGValue(bg), GetBValue(bg)});
    }
}

LRESULT CALLBACK ManagerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ManagerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ManagerWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            ResizeWebView();
            return 0;

        case WM_GETMINMAXINFO: {
            // Below this the two-pane layout starts overlapping itself. In CSS
            // pixels, like the default size, so it holds on a scaled display.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT dpi = DpiFor(hwnd_);
            mmi->ptMinTrackSize.x = Scaled(780, dpi);
            mmi->ptMinTrackSize.y = Scaled(540, dpi);
            return 0;
        }

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd_, &rc);
            HBRUSH brush = CreateSolidBrush(ThemeBackground(light_));
            FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
            DeleteObject(brush);
            return 1;
        }

        case WM_CLOSE:
            if (onClose_) onClose_();
            return 0;  // the owner decides whether to hide or quit

        case WM_DPICHANGED: {
            // Follow the system's suggested rectangle when the window moves to a
            // display with a different scale factor.
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ResizeWebView();
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
