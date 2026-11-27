#include "timer.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

#include "resource.h"

using namespace Gdiplus;

const wchar_t* const kTimerImage = L"widget:timer";

namespace timerface {
namespace {

// The original's layout, in pixels at 96 DPI.
constexpr int kCompactW = 162, kCompactH = 33;
constexpr int kExpandedW = 300, kExpandedH = 132;
constexpr RECT kCompactExit{4, 4, 66, 29};
constexpr int kDotsX = 151;
constexpr POINT kCompactTime{110, 16};
constexpr RECT kExit{10, 10, 72, 42};
constexpr RECT kPrinter{10, 50, 72, 108};
constexpr RECT kAccount{105, 96, 288, 120};
constexpr int kLabelRight = 288, kValueRight = 212;
constexpr int kRows[3] = {28, 54, 80};

const Color kBgCompact(255, 0x14, 0x14, 0x14);
const Color kBgExpanded(255, 0x33, 0x33, 0x33);
const Color kBorder(255, 0, 0, 0);
const Color kRed(255, 0xE8, 0x27, 0x1E);
const Color kRedHover(255, 0xFF, 0x3A, 0x30);
const Color kWhite(255, 255, 255, 255);
const Color kGrey(255, 0x8F, 0x8F, 0x8F);
const Color kBtn(255, 0x44, 0x44, 0x44);
const Color kBtnHover(255, 0x55, 0x55, 0x55);
const Color kBtnBorder(255, 0xDC, 0xDC, 0xDC);
const Color kPrinterBg(255, 0xED, 0xED, 0xED);
const Color kPrinterHover(255, 255, 255, 255);

const wchar_t* kFont = L"Tahoma";

RectF Scaled(const RECT& r, double s) {
    return RectF((REAL)(r.left * s), (REAL)(r.top * s), (REAL)((r.right - r.left) * s),
                 (REAL)((r.bottom - r.top) * s));
}

bool Inside(const RECT& r, double s, int x, int y) {
    return x >= r.left * s && x <= r.right * s && y >= r.top * s && y <= r.bottom * s;
}

void Box(Graphics& g, const RectF& r, const Color& fill, const Color& line) {
    SolidBrush b(fill);
    g.FillRectangle(&b, r);
    Pen p(line, 1.0f);
    g.DrawRectangle(&p, r.X, r.Y, r.Width - 1, r.Height - 1);
}

enum class Align { Center, Right };

// `rtl` for Hebrew: the paragraph runs right to left, so "יתרת הדפסות ₪0"
// keeps its amount on the left, as it reads.
void Text(Graphics& g, const std::wstring& text, double px, bool bold, const Color& color,
          const RectF& box, Align align, bool rtl) {
    Font font(kFont, (REAL)px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat fmt(rtl ? StringFormatFlagsDirectionRightToLeft : 0);
    fmt.SetLineAlignment(StringAlignmentCenter);
    if (align == Align::Center)
        fmt.SetAlignment(StringAlignmentCenter);
    else
        fmt.SetAlignment(rtl ? StringAlignmentNear : StringAlignmentFar);
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
    SolidBrush brush(color);
    g.DrawString(text.c_str(), (INT)text.size(), &font, box, &fmt, &brush);
}

// Text whose right edge is at x, centred on y.
RectF RightOf(double x, double y, double s) {
    const double w = 260 * s, h = 26 * s;
    return RectF((REAL)(x - w), (REAL)(y - h / 2), (REAL)w, (REAL)h);
}

void DrawPrinter(Graphics& g, double s, bool hover) {
    const RectF r = Scaled(kPrinter, s);
    const Color bg = hover ? kPrinterHover : kPrinterBg;
    Box(g, r, bg, kBorder);
    const double cx = r.X + r.Width / 2.0, cy = r.Y + r.Height / 2.0;
    const double u = std::min(r.Width, r.Height) / 44.0;
    SolidBrush ink(Color(255, 0, 0, 0)), paper(bg);
    auto rect = [&](double x1, double y1, double x2, double y2) {
        return RectF((REAL)x1, (REAL)y1, (REAL)(x2 - x1), (REAL)(y2 - y1));
    };
    g.FillRectangle(&ink, rect(cx - 8 * u, cy - 14 * u, cx + 8 * u, cy - 6 * u));   // paper in
    g.FillRectangle(&ink, rect(cx - 14 * u, cy - 6 * u, cx + 14 * u, cy + 6 * u));  // body
    g.FillRectangle(&paper, rect(cx + 8 * u, cy - 3 * u, cx + 12 * u, cy - 1 * u)); // light
    const RectF out = rect(cx - 9 * u, cy + 4 * u, cx + 9 * u, cy + 14 * u);          // paper out
    g.FillRectangle(&paper, out);
    Pen line(Color(255, 0, 0, 0), (REAL)std::max(1.0, u));
    g.DrawRectangle(&line, out);
}

const std::wstring kTextExit = L"יציאה";
const std::wstring kTextLeft = L"זמן שנשאר";
const std::wstring kTextUsed = L"זמן שימוש";
const std::wstring kTextPrints = L"יתרת הדפסות ₪0";
const std::wstring kTextAccount = L"החשבון שלי";

}  // namespace

SIZE Size(bool expanded, double s) {
    return expanded ? SIZE{(LONG)std::lround(kExpandedW * s), (LONG)std::lround(kExpandedH * s)}
                    : SIZE{(LONG)std::lround(kCompactW * s), (LONG)std::lround(kCompactH * s)};
}

Part HitTest(bool expanded, double s, int x, int y) {
    if (expanded) {
        if (Inside(kExit, s, x, y)) return Part::Exit;
        if (Inside(kAccount, s, x, y)) return Part::Account;
        if (Inside(kPrinter, s, x, y)) return Part::Printer;
        return Part::None;
    }
    if (Inside(kCompactExit, s, x, y)) return Part::Exit;
    if (Inside(RECT{kDotsX - 8, 4, kDotsX + 8, kCompactH - 4}, s, x, y)) return Part::Dots;
    return Part::None;
}

void Draw(Graphics& g, bool expanded, double s, Part hover, double remaining, double used) {
    // Every letter sits on an opaque fill, so ClearType is safe here, and it is
    // what the original's text looked like (thinner without it).
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const SIZE size = Size(expanded, s);

    if (!expanded) {
        Box(g, RectF(0, 0, (REAL)size.cx, (REAL)size.cy), kBgCompact, kBorder);
        const RectF exit = Scaled(kCompactExit, s);
        Box(g, exit, hover == Part::Exit ? kRedHover : kRed, kWhite);
        Text(g, kTextExit, 11 * s, true, kWhite, exit, Align::Center, true);
        const RectF time((REAL)((kCompactTime.x - 45) * s), 0, (REAL)(90 * s), (REAL)size.cy);
        Text(g, Format(remaining), 14 * s, true, remaining > 0 ? kWhite : kRedHover, time,
             Align::Center, false);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush dot(kWhite);
        const REAL r = (REAL)std::max(1.0, 1.9 * s);
        for (int y : {8, 16, 24})
            g.FillEllipse(&dot, (REAL)(kDotsX * s) - r, (REAL)(y * s) - r, 2 * r, 2 * r);
        g.SetSmoothingMode(SmoothingModeDefault);
        return;
    }

    {
        // The panel's rounded corners are see-through: the window is per-pixel alpha.
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        const REAL w = (REAL)size.cx - 1, h = (REAL)size.cy - 1, d = (REAL)(14 * s);
        GraphicsPath path;
        path.AddArc(0.0f, 0.0f, d, d, 180, 90);
        path.AddArc(w - d, 0.0f, d, d, 270, 90);
        path.AddArc(w - d, h - d, d, d, 0, 90);
        path.AddArc(0.0f, h - d, d, d, 90, 90);
        path.CloseFigure();
        SolidBrush fill(kBgExpanded);
        g.FillPath(&fill, &path);
        Pen line(kBorder, 1.0f);
        g.DrawPath(&line, &path);
        g.SetSmoothingMode(SmoothingModeDefault);
    }

    const RectF exit = Scaled(kExit, s);
    const Color red = hover == Part::Exit ? kRedHover : kRed;
    Box(g, exit, red, red);
    Text(g, kTextExit, 12 * s, true, kWhite, exit, Align::Center, true);

    DrawPrinter(g, s, hover == Part::Printer);

    struct Row { const std::wstring* label; std::wstring value; Color color; };
    const Row rows[3] = {
        {&kTextLeft, Format(remaining), kWhite},
        {&kTextUsed, Format(used), kGrey},
        {&kTextPrints, L"", kWhite},
    };
    for (int i = 0; i < 3; ++i) {
        Text(g, *rows[i].label, 12 * s, false, rows[i].color,
             RightOf(kLabelRight * s, kRows[i] * s, s), Align::Right, true);
        if (!rows[i].value.empty())
            Text(g, rows[i].value, 15 * s, true, rows[i].color,
                 RightOf(kValueRight * s, kRows[i] * s, s), Align::Right, false);
    }

    const RectF account = Scaled(kAccount, s);
    Box(g, account, hover == Part::Account ? kBtnHover : kBtn, kBtnBorder);
    Text(g, kTextAccount, 12 * s, false, kWhite, account, Align::Center, true);
}

std::unique_ptr<Bitmap> Preview() {
    const SIZE size = Size(false, 1.0);
    auto bmp = std::make_unique<Bitmap>(size.cx, size.cy, PixelFormat32bppARGB);
    Graphics g(bmp.get());
    g.Clear(Color(0, 0, 0, 0));
    Draw(g, false, 1.0, Part::None, kTimerDefaultStart, 0);
    return bmp;
}

std::wstring Format(double seconds) {
    const long long t = std::max(0LL, (long long)seconds);
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld", t / 3600, t % 3600 / 60, t % 60);
    return buf;
}

bool Parse(const std::wstring& text, int& seconds) {
    std::vector<long long> parts;
    std::wstring cur;
    bool any = false;
    auto flush = [&]() -> bool {
        if (cur.empty()) return true;  // "2:" and doubled separators are fine
        if (cur.size() > 6) return false;
        parts.push_back(std::stoll(cur));
        cur.clear();
        any = true;
        return true;
    };
    for (wchar_t ch : text) {
        if (ch >= L'0' && ch <= L'9') {
            cur += ch;
        } else if (ch == L':' || ch == L'.') {
            if (!flush()) return false;
        } else if (!std::iswspace(ch)) {
            return false;
        }
    }
    if (!flush() || !any || parts.size() > 3) return false;
    while (parts.size() < 3) parts.push_back(0);
    const long long total = parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (total > 999LL * 3600) return false;
    seconds = (int)total;
    return true;
}

// --- the time prompt ---------------------------------------------------------

namespace {

const wchar_t* kAskClass = L"FloatyAskTime";
constexpr int kEditId = 100;

struct Ask {
    std::wstring title;
    int initial = 0;
    int result = 0;
    bool ok = false;
    bool done = false;
    HFONT font = nullptr;
    HWND edit = nullptr;
};

UINT WindowDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    if (auto f = reinterpret_cast<Fn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")))
        if (UINT d = f(hwnd)) return d;
    return 96;
}

LRESULT CALLBACK AskProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ask = reinterpret_cast<Ask*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_COMMAND:
            if (!ask) break;
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[64] = {0};
                GetWindowTextW(ask->edit, buf, 64);
                int v = 0;
                if (!Parse(buf, v)) {
                    MessageBeep(MB_ICONWARNING);
                    SetFocus(ask->edit);
                    SendMessageW(ask->edit, EM_SETSEL, 0, -1);
                    return 0;
                }
                ask->result = v;
                ask->ok = true;
                ask->done = true;
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                ask->done = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            if (ask) ask->done = true;
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool AskTime(HWND owner, const std::wstring& title, int initial, int& out) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = AskProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kAskClass;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&wc);
        registered = true;
    }

    Ask ask;
    ask.title = title;
    ask.initial = initial;

    const UINT dpi = WindowDpi(owner);
    auto px = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    const int cw = px(280), ch = px(118);
    RECT frame{0, 0, cw, ch};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_LAYOUTRTL;
    AdjustWindowRectEx(&frame, style, FALSE, ex);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;

    // Centred on the monitor the timer is on.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    HWND dlg = CreateWindowExW(ex, kAskClass, L"Floaty", style,
                               wa.left + (wa.right - wa.left - w) / 2,
                               wa.top + (wa.bottom - wa.top - h) / 2, w, h, owner, nullptr,
                               inst, &ask);
    if (!dlg) return false;

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -px(13);
    ask.font = CreateFontIndirectW(&lf);
    LOGFONTW big = lf;
    big.lfHeight = -px(17);
    HFONT bigFont = CreateFontIndirectW(&big);

    const int pad = px(14);
    HWND label = CreateWindowExW(0, L"STATIC", (title + L"  (h:mm:ss)").c_str(),
                                 WS_CHILD | WS_VISIBLE | SS_RIGHT, pad, px(10), cw - 2 * pad,
                                 px(20), dlg, nullptr, inst, nullptr);
    ask.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Format(initial).c_str(),
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL,
                               pad + px(40), px(36), cw - 2 * pad - px(80), px(30), dlg,
                               reinterpret_cast<HMENU>((INT_PTR)kEditId), inst, nullptr);
    const int bw = px(90), bh = px(28), by = ch - bh - px(12);
    HWND okBtn = CreateWindowExW(0, L"BUTTON", L"אישור",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 cw / 2 - bw - px(4), by, bw, bh, dlg,
                                 reinterpret_cast<HMENU>((INT_PTR)IDOK), inst, nullptr);
    HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"ביטול",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     cw / 2 + px(4), by, bw, bh, dlg,
                                     reinterpret_cast<HMENU>((INT_PTR)IDCANCEL), inst, nullptr);
    for (HWND c : {label, okBtn, cancelBtn})
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ask.font), TRUE);
    SendMessageW(ask.edit, WM_SETFONT, reinterpret_cast<WPARAM>(bigFont), TRUE);

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(ask.edit);
    SendMessageW(ask.edit, EM_SETSEL, 0, -1);

    MSG m{};
    while (!ask.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!ask.done) PostQuitMessage((int)m.wParam);  // WM_QUIT arrived: pass it on

    SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
    DestroyWindow(dlg);
    DeleteObject(ask.font);
    DeleteObject(bigFont);
    if (ask.ok) out = ask.result;
    return ask.ok;
}

}  // namespace timerface
