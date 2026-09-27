#include "app.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <thread>

#include "json.h"
#include "resource.h"
#include "updater.h"
#include "util.h"

namespace {

const wchar_t* kHiddenClass = L"OverlayMgrAppWnd";
const wchar_t* kMutexName = L"OverlayManager_SingleInstanceMutex";
const wchar_t* kAppName = L"Overlay Manager";

const UINT WM_TRAY = WM_APP + 1;
const UINT WM_UPDATE_DONE = WM_APP + 2;   // lParam: UpdateResult*, owned by the receiver
const int kHotkeyId = 1;

// Update checks: shortly after start (not during it, so a slow network never
// delays the overlays), then every few hours for a machine that stays on.
const UINT_PTR kUpdateTimer = 1;
const UINT kFirstCheckMs = 20 * 1000;
const UINT kCheckEveryMs = 6 * 60 * 60 * 1000;

// Tray menu command ids.
enum {
    CMD_OPEN = 100,
    CMD_TOGGLE_ALL,
    CMD_EXIT,
};

// A colour the ChooseColor dialog remembers between invocations.
COLORREF g_customColors[16] = {0};

// Serialises one layer for the page. Field names match the model, so the
// JavaScript reads like the C++.
js::Value LayerToJson(const Layer& l) {
    js::Object o;
    o["name"] = ToUtf8(l.name);
    o["image"] = ToUtf8(l.image);
    o["enabled"] = l.enabled;
    o["width"] = l.width;
    o["height"] = l.height;
    o["lockAspect"] = l.lockAspect;
    o["monitor"] = l.monitor;
    o["anchorH"] = l.anchorH;
    o["anchorV"] = l.anchorV;
    o["offsetX"] = l.offsetX;
    o["offsetY"] = l.offsetY;
    o["opacity"] = l.opacity;
    o["useTint"] = l.useTint;
    o["tintR"] = l.tintR;
    o["tintG"] = l.tintG;
    o["tintB"] = l.tintB;
    o["rotation"] = l.rotation;
    o["clickThrough"] = l.clickThrough;
    o["grayscale"] = l.grayscale;
    o["removeBg"] = l.removeBg;
    o["bgTolerance"] = l.bgTolerance;

    // Native pixel size, so the aspect lock in the UI uses the real ratio
    // rather than whatever the box happens to be set to right now.
    int iw = 0, ih = 0;
    const bool readable = ImagePixelSize(l.image, iw, ih);
    if (readable) {
        o["imageW"] = iw;
        o["imageH"] = ih;
    }
    // A layer whose picture has been moved or deleted still has a path, and
    // used to render as an unexplained blank. Say so instead.
    o["missing"] = !l.image.empty() && !readable;
    // The file is there but no decoder could open it: a different fix than
    // re-picking it, so the page words it differently.
    o["unreadable"] = !l.image.empty() && !readable &&
                      GetFileAttributesW(l.image.c_str()) != INVALID_FILE_ATTRIBUTES;
    return js::Value(std::move(o));
}

// Applies a layer object from the page. Every field is optional: a missing or
// mistyped value keeps what was already there.
void ApplyLayerJson(const js::Value& v, Layer& l) {
    if (v.has("name")) l.name = ToWide(v["name"].asString());
    if (v.has("image")) l.image = ToWide(v["image"].asString());
    if (v.has("enabled")) l.enabled = v["enabled"].asBool(l.enabled);
    if (v.has("width")) l.width = std::max(16, v["width"].asInt(l.width));
    if (v.has("height")) l.height = std::max(16, v["height"].asInt(l.height));
    if (v.has("lockAspect")) l.lockAspect = v["lockAspect"].asBool(l.lockAspect);
    if (v.has("monitor")) l.monitor = std::max(0, v["monitor"].asInt(l.monitor));
    if (v.has("anchorH")) l.anchorH = std::clamp(v["anchorH"].asInt(l.anchorH), 0, 2);
    if (v.has("anchorV")) l.anchorV = std::clamp(v["anchorV"].asInt(l.anchorV), 0, 2);
    if (v.has("offsetX")) l.offsetX = v["offsetX"].asNumber(l.offsetX);
    if (v.has("offsetY")) l.offsetY = v["offsetY"].asNumber(l.offsetY);
    if (v.has("opacity")) l.opacity = std::clamp(v["opacity"].asInt(l.opacity), 0, 255);
    if (v.has("useTint")) l.useTint = v["useTint"].asBool(l.useTint);
    if (v.has("tintR")) l.tintR = std::clamp(v["tintR"].asInt(l.tintR), 0, 255);
    if (v.has("tintG")) l.tintG = std::clamp(v["tintG"].asInt(l.tintG), 0, 255);
    if (v.has("tintB")) l.tintB = std::clamp(v["tintB"].asInt(l.tintB), 0, 255);
    if (v.has("rotation")) l.rotation = std::clamp(v["rotation"].asNumber(l.rotation), -180.0, 180.0);
    if (v.has("clickThrough")) l.clickThrough = v["clickThrough"].asBool(l.clickThrough);
    if (v.has("grayscale")) l.grayscale = v["grayscale"].asBool(l.grayscale);
    if (v.has("removeBg")) l.removeBg = v["removeBg"].asBool(l.removeBg);
    if (v.has("bgTolerance")) l.bgTolerance = std::clamp(v["bgTolerance"].asInt(l.bgTolerance), 1, 100);
}

std::string HotkeyLabel(const Settings& s) {
    std::wstring out;
    if (s.hotkeyMods & MOD_CONTROL) out += L"Ctrl+";
    if (s.hotkeyMods & MOD_ALT) out += L"Alt+";
    if (s.hotkeyMods & MOD_SHIFT) out += L"Shift+";
    if (s.hotkeyMods & MOD_WIN) out += L"Win+";
    out += (wchar_t)s.hotkeyVk;
    return ToUtf8(out);
}

}  // namespace

// Handed from the worker thread back to the UI thread.
struct UpdateResult {
    bool manual = false;
    UpdateInfo info;
    std::string installError;
    bool installed = false;
};

App::App(HINSTANCE inst) : inst_(inst) {}

App::~App() {
    RemoveTrayIcon();
    if (hidden_) {
        UnregisterHotKey(hidden_, kHotkeyId);
        DestroyWindow(hidden_);
    }
    overlays_.clear();
    manager_.reset();
    GdiPlusStop();
    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
}

bool App::Start(bool startMinimized, bool afterUpdate) {
    // Single instance: a second launch pops the running one's window rather
    // than adding a duplicate set of overlays.
    mutex_ = CreateMutexW(nullptr, TRUE, kMutexName);
    bool taken = mutex_ && GetLastError() == ERROR_ALREADY_EXISTS;
    if (taken && afterUpdate) {
        // Launched by the updater while the old version is still shutting
        // down: wait for it to let go instead of handing over to it.
        const DWORD w = WaitForSingleObject(mutex_, 20000);
        taken = !(w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
    }
    if (taken) {
        if (HWND existing = FindWindowW(kHiddenClass, nullptr))
            PostMessageW(existing, WM_COMMAND, CMD_OPEN, 0);
        else
            MessageBoxW(nullptr, L"לא ניתן היה להחליף את המופע הקודם של התוכנה.",
                        kAppName, MB_ICONWARNING | MB_OK);
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }

    if (!GdiPlusStart()) {
        MessageBoxW(nullptr, L"לא ניתן היה לאתחל את GDI+.", kAppName, MB_ICONERROR);
        return false;
    }

    RefreshMonitors();
    settings_.hotkeyMods = MOD_CONTROL | MOD_ALT;
    settings_.hotkeyVk = 'H';
    LoadConfig(settings_, layers_);

    // The registry is the source of truth for autostart: the user may have
    // removed the entry through Task Manager since the config was written.
    settings_.autoStart = IsAutoStartEnabled();

    if (!layers_.empty()) selected_ = 0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = &App::HiddenProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kHiddenClass;
    RegisterClassW(&wc);
    hidden_ = CreateWindowExW(0, kHiddenClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                              nullptr, inst_, this);
    if (!hidden_) return false;
    SetWindowLongPtrW(hidden_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    OverlayWindow::RegisterWindowClass(inst_);
    ManagerWindow::RegisterWindowClass(inst_);

    RegisterHotkey();
    if (settings_.showTrayIcon) CreateTrayIcon();
    RebuildOverlays();

    manager_ = std::make_unique<ManagerWindow>(
        inst_, [this](const std::string& json) { OnUiMessage(json); },
        [this] {
            // Closing the window keeps the overlays running, unless there is no
            // tray icon to get back in through.
            if (settings_.minimizeToTray && settings_.showTrayIcon)
                manager_->Hide();
            else
                RequestQuit();
        });

    if (!manager_->Create(settings_.lightTheme)) return false;
    if (!startMinimized) manager_->Show();

    justUpdated_ = afterUpdate;
    CleanupAfterUpdate();
    SetTimer(hidden_, kUpdateTimer, kFirstCheckMs, nullptr);
    return true;
}

int App::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Let the WebView handle its own accelerators and tab order.
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

// --- overlays --------------------------------------------------------------

void App::RebuildOverlays() {
    overlays_.clear();
    overlays_.reserve(layers_.size());
    for (size_t i = 0; i < layers_.size(); ++i) {
        overlays_.push_back(std::make_unique<OverlayWindow>(
            inst_, (int)i, layers_[i],
            [this](int index, const Geometry& g) {
                // The user dragged the overlay itself; mirror it back into the
                // model and the UI so both stay in step.
                if (index < 0 || index >= (int)layers_.size()) return;
                Layer& l = layers_[index];
                l.offsetX = g.offsetX;
                l.offsetY = g.offsetY;
                l.width = g.width;
                l.height = g.height;
                SendLayers(selected_);
            }));
        overlays_.back()->SetHiddenByHotkey(allHidden_);
    }
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size())
        overlays_[positionIndex_]->SetPositionMode(true);
}

void App::SyncOverlay(int index) {
    if (index < 0 || index >= (int)overlays_.size()) return;
    overlays_[index]->Update(layers_[index]);
}

void App::SetAllHidden(bool hidden) {
    allHidden_ = hidden;
    for (auto& o : overlays_) o->SetHiddenByHotkey(hidden);
    UpdateTrayIcon();

    js::Object m;
    m["t"] = "hiddenAll";
    m["on"] = allHidden_;
    Send(js::Value(std::move(m)).dump());
}

void App::SetPositionMode(int index, bool on) {
    // Only one layer at a time is draggable, otherwise the scrims stack up and
    // the wrong one takes the click.
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size())
        overlays_[positionIndex_]->SetPositionMode(false);

    positionIndex_ = on ? index : -1;
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size()) {
        overlays_[positionIndex_]->SetPositionMode(true);
        // A hidden or disabled layer cannot be dragged, so make it visible for
        // the duration instead of leaving the user clicking at nothing.
        if (allHidden_) SetAllHidden(false);
        if (!layers_[positionIndex_].enabled) {
            layers_[positionIndex_].enabled = true;
            SyncOverlay(positionIndex_);
            SendLayers(selected_);
        }
    }

    js::Object m;
    m["t"] = "positionMode";
    m["on"] = on;
    m["i"] = index;
    Send(js::Value(std::move(m)).dump());
}

// --- messages to the page --------------------------------------------------

void App::Send(const std::string& json) {
    if (manager_) manager_->Post(json);
}

void App::SendInit() {
    js::Object m;
    m["t"] = "init";

    js::Object s;
    s["autoStart"] = settings_.autoStart;
    s["minimizeToTray"] = settings_.minimizeToTray;
    s["showTrayIcon"] = settings_.showTrayIcon;
    s["lightTheme"] = settings_.lightTheme;
    s["autoUpdate"] = settings_.autoUpdate;
    s["hotkey"] = HotkeyLabel(settings_);
    m["settings"] = js::Value(std::move(s));

    js::Array mons;
    for (const auto& mon : Monitors()) {
        js::Object o;
        o["label"] = ToUtf8(mon.label);
        o["width"] = mon.width;
        o["height"] = mon.height;
        mons.push_back(js::Value(std::move(o)));
    }
    m["monitors"] = js::Value(std::move(mons));

    js::Array arr;
    for (const auto& l : layers_) arr.push_back(LayerToJson(l));
    m["layers"] = js::Value(std::move(arr));
    m["selected"] = selected_;
    m["version"] = AppVersion();

    Send(js::Value(std::move(m)).dump());
    SendUpdateState();
    if (justUpdated_) {
        justUpdated_ = false;
        js::Object t;
        t["t"] = "toast";
        t["text"] = std::string("התוכנה עודכנה לגרסה ") + AppVersion();
        Send(js::Value(std::move(t)).dump());
    }
}

void App::SendUpdateState() {
    js::Object m;
    m["t"] = "update";
    m["busy"] = updateBusy_;
    m["ready"] = updateReady_;
    Send(js::Value(std::move(m)).dump());
}

void App::SendLayers(int selected) {
    js::Object m;
    m["t"] = "layers";
    js::Array arr;
    for (const auto& l : layers_) arr.push_back(LayerToJson(l));
    m["layers"] = js::Value(std::move(arr));
    m["selected"] = selected;
    Send(js::Value(std::move(m)).dump());
}

void App::SendPreview(const std::wstring& path, int bgTolerance) {
    js::Object m;
    m["t"] = "preview";
    m["path"] = ToUtf8(path);
    // Must match previewKey() in the page.
    m["key"] = ToUtf8(path) + (bgTolerance > 0 ? "|" + std::to_string(bgTolerance) : "");
    // 320px is plenty for the sidebar thumbnail and the mini-map, and keeps the
    // data URI small enough to post on every selection change.
    m["uri"] = ImageToDataUri(path, 320, bgTolerance);
    Send(js::Value(std::move(m)).dump());
}

// --- messages from the page ------------------------------------------------

void App::OnUiMessage(const std::string& json) {
    const js::Value msg = js::Value::parse(json);
    const std::string cmd = msg["c"].asString();
    const int i = msg["i"].asInt(-1);
    const bool valid = i >= 0 && i < (int)layers_.size();

    if (cmd == "ready") {
        SendInit();
        return;
    }

    if (cmd == "select") {
        if (valid) selected_ = i;
        return;
    }

    if (cmd == "patch") {
        if (!valid) return;
        ApplyLayerJson(msg["layer"], layers_[i]);
        SyncOverlay(i);
        return;
    }

    if (cmd == "add") {
        Layer l;
        l.name = L"שכבה " + std::to_wstring(layers_.size() + 1);
        layers_.push_back(l);
        selected_ = (int)layers_.size() - 1;
        RebuildOverlays();
        SendLayers(selected_);
        return;
    }

    if (cmd == "duplicate") {
        if (!valid) return;
        Layer copy = layers_[i];
        copy.name += L" (עותק)";
        // Nudge the copy so it does not land exactly on the original and look
        // like nothing happened.
        copy.offsetX += 2.0;
        copy.offsetY += 2.0;
        layers_.insert(layers_.begin() + i + 1, copy);
        selected_ = i + 1;
        RebuildOverlays();
        SendLayers(selected_);
        return;
    }

    if (cmd == "delete") {
        if (!valid) return;
        const std::wstring prompt =
            L"למחוק את השכבה \"" + layers_[i].name + L"\"?\n\nאי אפשר לבטל את הפעולה.";
        if (MessageBoxW(manager_ ? manager_->Hwnd() : nullptr, prompt.c_str(),
                        kAppName, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            return;
        layers_.erase(layers_.begin() + i);
        if (positionIndex_ == i) positionIndex_ = -1;
        selected_ = layers_.empty() ? -1 : std::min(i, (int)layers_.size() - 1);
        RebuildOverlays();
        SendLayers(selected_);
        return;
    }

    if (cmd == "move") {
        const int from = msg["from"].asInt(-1);
        int to = msg["to"].asInt(-1);
        if (from < 0 || from >= (int)layers_.size()) return;
        to = std::clamp(to, 0, (int)layers_.size() - 1);
        if (from == to) return;
        Layer l = layers_[from];
        layers_.erase(layers_.begin() + from);
        layers_.insert(layers_.begin() + to, l);
        selected_ = to;
        RebuildOverlays();
        SendLayers(selected_);
        return;
    }

    if (cmd == "needPreview") {
        SendPreview(ToWide(msg["path"].asString()), std::clamp(msg["bg"].asInt(0), 0, 100));
        return;
    }

    if (cmd == "browse") {
        if (valid) BrowseImage(i);
        return;
    }

    if (cmd == "color") {
        if (valid) PickTint(i);
        return;
    }

    if (cmd == "position") {
        if (valid) SetPositionMode(i, msg["on"].asBool(true));
        return;
    }

    if (cmd == "toggleAll") {
        SetAllHidden(!allHidden_);
        return;
    }

    if (cmd == "settings") {
        const js::Value s = msg["settings"];
        const bool wantAutoStart = s["autoStart"].asBool(settings_.autoStart);
        if (wantAutoStart != settings_.autoStart) {
            SetAutoStartEnabled(wantAutoStart);
            // Read it back: the write can fail silently under a locked-down
            // policy, and the checkbox should show what is really there.
            settings_.autoStart = IsAutoStartEnabled();
            if (settings_.autoStart != wantAutoStart) {
                js::Object t;
                t["t"] = "toast";
                t["text"] = "לא ניתן היה לשנות את ההפעלה האוטומטית";
                t["warn"] = true;
                Send(js::Value(std::move(t)).dump());
            }
        }
        settings_.minimizeToTray = s["minimizeToTray"].asBool(settings_.minimizeToTray);
        settings_.lightTheme = s["lightTheme"].asBool(settings_.lightTheme);
        settings_.autoUpdate = s["autoUpdate"].asBool(settings_.autoUpdate);

        const bool wantTray = s["showTrayIcon"].asBool(settings_.showTrayIcon);
        if (wantTray != settings_.showTrayIcon) {
            settings_.showTrayIcon = wantTray;
            if (wantTray)
                CreateTrayIcon();
            else
                RemoveTrayIcon();
        }
        return;
    }

    if (cmd == "save") {
        if (SaveConfig(settings_, layers_)) {
            js::Object m;
            m["t"] = "saved";
            Send(js::Value(std::move(m)).dump());
        } else {
            js::Object m;
            m["t"] = "toast";
            m["text"] = "השמירה נכשלה — אין הרשאת כתיבה לתיקיית ההגדרות";
            m["warn"] = true;
            Send(js::Value(std::move(m)).dump());
        }
        return;
    }

    if (cmd == "checkUpdate") {
        StartUpdateCheck(true);
        return;
    }

    if (cmd == "restartUpdate") {
        RestartIntoUpdate();
        return;
    }

    if (cmd == "exit") {
        RequestQuit();
        return;
    }
}

// --- dialogs ---------------------------------------------------------------

void App::BrowseImage(int index) {
    wchar_t file[MAX_PATH * 4] = {0};
    wcsncpy_s(file, layers_[index].image.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = manager_ ? manager_->Hwnd() : nullptr;
    ofn.lpstrFilter =
        L"תמונות (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif)\0"
        L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff\0"
        L"כל הקבצים\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;

    Layer& l = layers_[index];
    l.image = file;

    // Fit the box to the new image's aspect ratio, keeping the current width.
    int iw = 0, ih = 0;
    if (ImagePixelSize(l.image, iw, ih) && iw > 0) {
        if (l.lockAspect)
            l.height = std::max(16, (int)std::lround((double)l.width * ih / iw));
        // A layer that never had a picture starts life at the image's own size,
        // clamped so a huge photo does not cover the whole screen.
        if (l.name == L"שכבה חדשה" || (l.width == 200 && l.height == 200)) {
            const int cap = 480;
            const double scale = std::min(1.0, (double)cap / std::max(iw, ih));
            l.width = std::max(16, (int)std::lround(iw * scale));
            l.height = std::max(16, (int)std::lround(ih * scale));
        }
    }

    SyncOverlay(index);
    SendPreview(l.image, BgKey(l));
    SendLayers(selected_);
}

void App::PickTint(int index) {
    Layer& l = layers_[index];

    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = manager_ ? manager_->Hwnd() : nullptr;
    cc.rgbResult = RGB(l.tintR, l.tintG, l.tintB);
    cc.lpCustColors = g_customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;

    if (!ChooseColorW(&cc)) return;

    l.tintR = GetRValue(cc.rgbResult);
    l.tintG = GetGValue(cc.rgbResult);
    l.tintB = GetBValue(cc.rgbResult);
    // Picking a colour implies wanting it applied.
    l.useTint = true;

    SyncOverlay(index);
    SendLayers(selected_);
}

// --- shell integration -----------------------------------------------------

bool App::CreateTrayIcon() {
    if (trayAdded_) return true;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hidden_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APP));
    wcsncpy_s(nid.szTip, kAppName, _TRUNCATE);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return trayAdded_;
}

void App::RemoveTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hidden_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void App::UpdateTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hidden_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    // The tooltip is the only place the hidden state is visible when the
    // manager window is closed.
    const std::wstring tip =
        allHidden_ ? std::wstring(kAppName) + L" — השכבות מוסתרות" : kAppName;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, CMD_OPEN, L"פתח חלון ניהול");
    AppendMenuW(menu, MF_STRING, CMD_TOGGLE_ALL,
                allHidden_ ? L"הצג הכל" : L"הסתר הכל");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"יציאה");

    POINT pt;
    GetCursorPos(&pt);
    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(hidden_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RIGHTALIGN, pt.x, pt.y, 0, hidden_,
                   nullptr);
    PostMessageW(hidden_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void App::RegisterHotkey() {
    UnregisterHotKey(hidden_, kHotkeyId);
    if (!RegisterHotKey(hidden_, kHotkeyId, settings_.hotkeyMods, settings_.hotkeyVk)) {
        // Another program already owns the combination. Not fatal: everything
        // else still works, so say so once instead of refusing to start.
        js::Object m;
        m["t"] = "toast";
        m["text"] = "צירוף המקשים " + HotkeyLabel(settings_) + " תפוס על ידי תוכנה אחרת";
        m["warn"] = true;
        Send(js::Value(std::move(m)).dump());
    }
}

// --- updates ---------------------------------------------------------------

void App::StartUpdateCheck(bool manual) {
    if (updateBusy_) return;
    if (!updateReady_.empty()) {
        // Already downloaded; checking again would only fetch the same file.
        if (manual) SendUpdateState();
        return;
    }
    updateBusy_ = true;
    if (manual) SendUpdateState();

    const HWND target = hidden_;
    std::thread([target, manual] {
        auto* r = new UpdateResult;
        r->manual = manual;
        r->info = CheckForUpdate();
        if (r->info.ok && r->info.newer) {
            r->installError = DownloadAndInstall(r->info);
            r->installed = r->installError.empty();
        }
        if (!PostMessageW(target, WM_UPDATE_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void App::OnUpdateFinished(UpdateResult* result) {
    std::unique_ptr<UpdateResult> r(result);
    updateBusy_ = false;

    std::string text;
    bool warn = false;
    if (r->installed) {
        updateReady_ = r->info.version;
        // Nobody is looking at the window: restart straight away, the overlays
        // come back within a second. Otherwise leave the choice to the user.
        if (!manager_ || !manager_->IsVisible()) {
            RestartIntoUpdate();
            return;
        }
    } else if (!r->info.ok) {
        text = r->info.error;
        warn = true;
    } else if (!r->installError.empty()) {
        text = "העדכון לגרסה " + r->info.version + " נכשל: " + r->installError;
        warn = true;
    } else if (!r->info.error.empty()) {
        text = r->info.error;
        warn = true;
    } else {
        text = std::string("זו הגרסה העדכנית ביותר (") + AppVersion() + ")";
    }

    SendUpdateState();
    // A background check stays quiet unless it has something to show; a
    // failed install is worth knowing about either way.
    if (!text.empty() && (r->manual || !r->installError.empty())) {
        js::Object t;
        t["t"] = "toast";
        t["text"] = text;
        t["warn"] = warn;
        Send(js::Value(std::move(t)).dump());
    }
}

void App::RestartIntoUpdate() {
    if (updateReady_.empty()) return;
    const bool minimized = !manager_ || !manager_->IsVisible();
    if (!RelaunchUpdated(minimized)) {
        js::Object t;
        t["t"] = "toast";
        t["text"] = "לא ניתן היה להפעיל מחדש — הגרסה החדשה תיפתח בהפעלה הבאה";
        t["warn"] = true;
        Send(js::Value(std::move(t)).dump());
        return;
    }
    RequestQuit();
}

void App::ShowManager() {
    if (manager_) manager_->Show();
}

void App::RequestQuit() {
    quitting_ = true;
    // Persist before leaving, so a session's tweaks are not silently lost.
    SaveConfig(settings_, layers_);
    RemoveTrayIcon();
    overlays_.clear();
    PostQuitMessage(0);
}

// --- hidden window ---------------------------------------------------------

LRESULT CALLBACK App::HiddenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP)
                ShowManager();
            else if (LOWORD(lp) == WM_RBUTTONUP)
                ShowTrayMenu();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case CMD_OPEN: ShowManager(); return 0;
                case CMD_TOGGLE_ALL: SetAllHidden(!allHidden_); return 0;
                case CMD_EXIT: RequestQuit(); return 0;
                default: break;
            }
            break;

        case WM_UPDATE_DONE:
            OnUpdateFinished(reinterpret_cast<UpdateResult*>(lp));
            return 0;

        case WM_TIMER:
            if (wp == kUpdateTimer) {
                SetTimer(hidden_, kUpdateTimer, kCheckEveryMs, nullptr);
                if (settings_.autoUpdate) StartUpdateCheck(false);
                return 0;
            }
            break;

        case WM_HOTKEY:
            if (wp == kHotkeyId) SetAllHidden(!allHidden_);
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            // A monitor was added, removed or rearranged. Re-read the layout and
            // move every overlay onto its target screen's new rectangle.
            RefreshMonitors();
            const int count = (int)Monitors().size();
            for (auto& l : layers_)
                if (l.monitor >= count) l.monitor = 0;
            for (auto& o : overlays_) o->Reposition();
            for (size_t i = 0; i < overlays_.size(); ++i) SyncOverlay((int)i);
            SendInit();
            return 0;
        }

        case WM_ENDSESSION:
            // Windows is shutting down; there is no second chance to write.
            if (wp) SaveConfig(settings_, layers_);
            return 0;

        case WM_DESTROY:
            if (!quitting_) PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hidden_, msg, wp, lp);
}
