#include "app.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "json.h"
#include "resource.h"
#include "timer.h"
#include "updater.h"
#include "util.h"

namespace {

const wchar_t* kHiddenClass = L"FloatyAppWnd";
const wchar_t* kMutexName = L"Floaty_SingleInstanceMutex";
const wchar_t* kAppName = L"Floaty";

const UINT WM_TRAY = WM_APP + 1;
const UINT WM_UPDATE_DONE = WM_APP + 2;   // lParam: UpdateResult*, owned by the receiver
const int kHotkeyId = 1;        // hide / show everything
const int kCycleHotkeyId = 2;   // show the next layer, hide the others

// Broadcast by Explorer when the taskbar is (re)created, e.g. after Explorer
// crashed or was restarted: every tray icon has to be added again.
const UINT WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");

// Update checks: shortly after start (not during it, so a slow network never
// delays the overlays), then every few hours for a machine that stays on.
const UINT_PTR kUpdateTimer = 1;
const UINT kFirstCheckMs = 20 * 1000;
const UINT kCheckEveryMs = 6 * 60 * 60 * 1000;

// Usage statistics: how long each layer has been on screen, counted every few
// seconds and saved every few minutes (and on every save and on exit).
const UINT_PTR kUsageTimer = 2;
const UINT kUsageEveryMs = 15 * 1000;
const int kUsageSaveEveryTicks = 20;   // 20 × 15 s = 5 minutes

// The settings a first run starts with, and "Reset settings" returns to.
Settings DefaultSettings() {
    Settings s;  // the struct's own defaults: no autostart, tray icon on, dark, auto update
    s.hotkeyMods = MOD_CONTROL | MOD_ALT;
    s.hotkeyVk = 'H';
    // N for "next". Ctrl+Alt is AltGr on a Hebrew keyboard, and AltGr+N types
    // nothing there, so the combination takes no character away from anyone.
    s.cycleMods = MOD_CONTROL | MOD_ALT;
    s.cycleVk = 'N';
    return s;
}

std::string TodayIso() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[16];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return buf;
}

// Tray menu command ids.
enum {
    CMD_OPEN = 100,
    CMD_TOGGLE_ALL,
    CMD_EXIT,
    CMD_NEXT_LAYER,
};

// A colour the ChooseColor dialog remembers between invocations.
COLORREF g_customColors[16] = {0};

// Hebrew text in a message box needs right-to-left reading order and right
// alignment, or the punctuation lands on the wrong side and the text hugs the
// left edge.
int HebrewMessageBox(HWND owner, const wchar_t* text, UINT flags) {
    return MessageBoxW(owner, text, kAppName, flags | MB_RTLREADING | MB_RIGHT);
}

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
    if (IsTimerImage(l.image)) {
        js::Object t;
        t["start"] = l.timerStart;
        t["remaining"] = l.timerRemaining;
        t["used"] = l.timerUsed;
        t["running"] = l.timerRunning;
        t["locked"] = l.timerLocked;
        o["timer"] = js::Value(std::move(t));
    }

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

// The key's own name: letters and digits as they are, F-keys numbered, and
// the rest (arrows, Home, Space...) as the keyboard driver names them. A plain
// cast of the virtual-key code to a character made F1 read "p".
std::wstring VkName(unsigned vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {  // keys on the extended part of the keyboard
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
        case VK_HOME: case VK_END: case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
            scan |= 0x100; break;
        default: break;
    }
    wchar_t buf[64] = {0};
    if (scan && GetKeyNameTextW((LONG)(scan << 16), buf, 64) > 0) return buf;
    return L"#" + std::to_wstring(vk);
}

std::string HotkeyLabel(unsigned mods, unsigned vk) {
    if (!vk) return {};
    std::wstring out;
    if (mods & MOD_CONTROL) out += L"Ctrl+";
    if (mods & MOD_ALT) out += L"Alt+";
    if (mods & MOD_SHIFT) out += L"Shift+";
    if (mods & MOD_WIN) out += L"Win+";
    out += VkName(vk);
    return ToUtf8(out);
}

std::string HotkeyLabel(const Settings& s) { return HotkeyLabel(s.hotkeyMods, s.hotkeyVk); }

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
            HebrewMessageBox(nullptr, L"לא ניתן היה להחליף את המופע הקודם של התוכנה.",
                             MB_ICONWARNING | MB_OK);
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }

    if (!GdiPlusStart()) {
        HebrewMessageBox(nullptr, L"לא ניתן היה לאתחל את GDI+.", MB_ICONERROR);
        return false;
    }

    RefreshMonitors();
    settings_ = DefaultSettings();
    if (!LoadConfig(settings_, layers_) || settings_.statsSince.empty())
        settings_.statsSince = TodayIso();  // usage is counted from the first run

    // The registry is the source of truth for autostart: the user may have
    // removed the entry through Task Manager since the config was written.
    settings_.autoStart = IsAutoStartEnabled();
    // The entry names a path. If the program has since been moved and that
    // file is gone, autostart would fail silently at the next logon: point
    // it here. (A copy elsewhere that still exists is left alone, so running
    // a second copy once does not take the entry over.)
    if (settings_.autoStart) {
        const std::wstring target = AutoStartTarget();
        if (!target.empty() && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
            SetAutoStartEnabled(true);
    }

    if (!layers_.empty()) selected_ = 0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = &App::HiddenProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kHiddenClass;
    RegisterClassW(&wc);
    // A top-level window that is never shown, not a message-only one
    // (HWND_MESSAGE): message-only windows receive no broadcasts, so
    // WM_DISPLAYCHANGE, WM_ENDSESSION and TaskbarCreated would never arrive.
    hidden_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHiddenClass, kAppName, WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, inst_, this);
    if (!hidden_) return false;
    SetWindowLongPtrW(hidden_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Run elevated, the app would otherwise never hear Explorer's broadcast.
    ChangeWindowMessageFilterEx(hidden_, WM_TASKBAR_CREATED, MSGFLT_ALLOW, nullptr);

    OverlayWindow::RegisterWindowClass(inst_);
    ManagerWindow::RegisterWindowClass(inst_);

    if (settings_.showTrayIcon) CreateTrayIcon();
    RebuildOverlays();

    manager_ = std::make_unique<ManagerWindow>(
        inst_, [this](const std::string& json) { OnUiMessage(json); },
        [this] {
            // Positioning is driven from this window; left on, the layer would
            // stay a grabbable, click-blocking box with nothing to end it.
            if (positionIndex_ >= 0) SetPositionMode(positionIndex_, false);
            // Closing the window keeps the overlays running, unless there is no
            // tray icon to get back in through.
            if (settings_.minimizeToTray && settings_.showTrayIcon)
                manager_->Hide();
            else
                RequestQuit();
        });

    if (!manager_->Create(settings_.lightTheme)) return false;
    if (!startMinimized) manager_->Show();

    // After the manager exists: a clash with another program's hotkey is
    // reported to the page, and Send() drops messages while there is no page.
    RegisterHotkey();

    // An update left a note for this start: either it worked (this is the
    // version it installed) or it did not take, and either way the page says so.
    // Only a note about this very file counts: another copy of the program in
    // another folder shares the settings folder but was not the one replaced.
    const UpdateRecord record = TakeUpdateRecord();
    if (record.present && _wcsicmp(ToWide(record.exe).c_str(), ExePath().c_str()) == 0) {
        if (record.to == AppVersion())
            updated_ = record;
        else
            updateFailedTo_ = record.to;
    }
    (void)afterUpdate;
    CleanupAfterUpdate();
    SetTimer(hidden_, kUpdateTimer, kFirstCheckMs, nullptr);
    lastUsageTick_ = GetTickCount64();
    SetTimer(hidden_, kUsageTimer, kUsageEveryMs, nullptr);
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
            },
            TimerHooks{
                [this](int index, const Layer& t, bool save) { OnTimerChanged(index, t, save); },
                [this] { return settings_.autoStart; },
                [this] {
                    SetAutoStartEnabled(!settings_.autoStart);
                    settings_.autoStart = IsAutoStartEnabled();
                    SaveConfig(settings_, layers_);
                    SendSettings();
                },
                [this](int index) {
                    if (index >= 0 && index < (int)layers_.size()) {
                        selected_ = index;
                        SendLayers(selected_);
                    }
                    ShowManager();
                }}));
        overlays_.back()->SetHiddenByHotkey(allHidden_);
    }
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size())
        overlays_[positionIndex_]->SetPositionMode(true);
    RestackOverlays();
}

void App::RestackOverlays() {
    // The list order is the paint order: a later layer covers an earlier one.
    // Raising each window in turn to the top of the topmost band leaves them
    // stacked exactly that way.
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    for (auto& o : overlays_) SetWindowPos(o->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, flags);
    // While a layer is being positioned it has to be grabbable, even where a
    // later layer would cover it.
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size())
        SetWindowPos(overlays_[positionIndex_]->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, flags);
}

void App::OnTimerChanged(int index, const Layer& t, bool save) {
    if (index < 0 || index >= (int)layers_.size()) return;
    Layer& l = layers_[index];
    if (!IsTimerImage(l.image)) return;
    l.timerStart = t.timerStart;
    l.timerRemaining = t.timerRemaining;
    l.timerUsed = t.timerUsed;
    l.timerRunning = t.timerRunning;
    l.timerLocked = t.timerLocked;
    l.offsetX = t.offsetX;
    l.offsetY = t.offsetY;
    if (save) {
        SaveConfig(settings_, layers_);
        SendLayers(selected_);
    } else if (manager_ && manager_->IsVisible()) {
        // Once a second, for the clock in the editor.
        js::Object m;
        m["t"] = "timer";
        m["i"] = index;
        m["remaining"] = l.timerRemaining;
        m["used"] = l.timerUsed;
        m["running"] = l.timerRunning;
        Send(js::Value(std::move(m)).dump());
    }
}

void App::SyncOverlay(int index) {
    if (index < 0 || index >= (int)overlays_.size()) return;
    overlays_[index]->Update(layers_[index]);
}

void App::AccountUsage() {
    // Time since the last count goes to every layer that was on screen for it.
    // Called before anything that changes what is shown, so a toggle does not
    // credit (or lose) up to a tick's worth of the wrong state.
    const unsigned long long now = GetTickCount64();
    const double dt = (now - lastUsageTick_) / 1000.0;
    lastUsageTick_ = now;
    if (dt <= 0 || allHidden_) return;
    for (auto& l : layers_)
        if (l.enabled && !l.image.empty()) l.usageSeconds += dt;
}

void App::SetAllHidden(bool hidden) {
    AccountUsage();
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
    const int previous = positionIndex_;
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size())
        overlays_[positionIndex_]->SetPositionMode(false);

    positionIndex_ = on ? index : -1;
    if (positionIndex_ >= 0 && positionIndex_ < (int)overlays_.size()) {
        // What "Cancel" goes back to: the layer as it was, and whether
        // everything was hidden, before positioning switched those on.
        if (positionIndex_ != previous) {
            positionBackup_ = layers_[positionIndex_];
            positionWasHidden_ = allHidden_;
        }
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
    RestackOverlays();

    js::Object m;
    m["t"] = "positionMode";
    m["on"] = on;
    m["i"] = index;
    Send(js::Value(std::move(m)).dump());
}

void App::CancelPositionMode() {
    const int i = positionIndex_;
    if (i < 0 || i >= (int)layers_.size()) return;
    // Put back where, how big and whether it was shown; any other edit made
    // meanwhile (opacity, tint...) came from the editor and stays.
    Layer& l = layers_[i];
    l.offsetX = positionBackup_.offsetX;
    l.offsetY = positionBackup_.offsetY;
    l.width = positionBackup_.width;
    l.height = positionBackup_.height;
    l.enabled = positionBackup_.enabled;
    SetPositionMode(i, false);
    SyncOverlay(i);
    if (positionWasHidden_) SetAllHidden(true);
    SendLayers(selected_);
}

void App::RemapPositionIndex(int index) {
    // Rows above the positioned layer came or went: follow the same layer to
    // its new index, or leave position mode if it was the one removed. The
    // page is told, since it marks the layer by index.
    if (index == positionIndex_) return;
    positionIndex_ = index;
    js::Object m;
    m["t"] = "positionMode";
    m["on"] = index >= 0;
    m["i"] = index;
    Send(js::Value(std::move(m)).dump());
}

// --- messages to the page --------------------------------------------------

void App::Send(const std::string& json) {
    if (manager_) manager_->Post(json);
}

js::Value App::SettingsJson() const {
    js::Object s;
    s["autoStart"] = settings_.autoStart;
    s["minimizeToTray"] = settings_.minimizeToTray;
    s["showTrayIcon"] = settings_.showTrayIcon;
    s["lightTheme"] = settings_.lightTheme;
    s["autoUpdate"] = settings_.autoUpdate;
    s["hotkey"] = HotkeyLabel(settings_);
    s["cycleHotkey"] = HotkeyLabel(settings_.cycleMods, settings_.cycleVk);
    // Shown in the status bar for as long as it is true, not only as a toast
    // that a window opened later never sees.
    s["hotkeyBusy"] = !hotkeyOk_;
    s["cycleBusy"] = settings_.cycleVk != 0 && !cycleOk_;
    return js::Value(std::move(s));
}

void App::SendSettings() {
    js::Object m;
    m["t"] = "settings";
    m["settings"] = SettingsJson();
    Send(js::Value(std::move(m)).dump());
}

void App::SendInit() {
    js::Object m;
    m["t"] = "init";
    m["settings"] = SettingsJson();

    js::Array stickers, more;
    for (const auto& name : StickerNames(StickerGroup::Main)) stickers.push_back(js::Value(ToUtf8(name)));
    for (const auto& name : StickerNames(StickerGroup::More)) more.push_back(js::Value(ToUtf8(name)));
    m["stickers"] = js::Value(std::move(stickers));
    m["moreStickers"] = js::Value(std::move(more));

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
}

void App::SendUpdateState() {
    // Everything the page needs to say about updates, as state rather than
    // toasts: a window opened hours after the event still shows it.
    js::Object m;
    m["t"] = "update";
    m["busy"] = updateBusy_;
    m["ready"] = updateReady_;          // installed, waiting for a restart
    m["available"] = updateAvailable_;  // found, automatic install is off
    m["notes"] = updateNotes_;          // what the ready/available release says
    m["error"] = updateError_;          // the last install attempt failed
    m["updatedTo"] = updated_.to;       // this start is the result of an update
    m["updatedNotes"] = updated_.notes;
    m["failedTo"] = updateFailedTo_;    // an update was installed but did not take
    // The last check, for the details line in the settings: when, what the
    // newest release is and when it came out, or why the check failed.
    m["current"] = AppVersion();
    m["checkedAt"] = (double)lastCheck_.atMs;  // 0 = not checked since start
    m["checkOk"] = lastCheck_.ok;
    m["latest"] = lastCheck_.latest;
    m["published"] = lastCheck_.published;
    m["checkError"] = lastCheck_.error;
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
        AccountUsage();  // before a change that may hide or show the layer
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
        if (positionIndex_ > i) RemapPositionIndex(positionIndex_ + 1);
        RebuildOverlays();
        SendLayers(selected_);
        return;
    }

    if (cmd == "delete") {
        if (!valid) return;
        // The page asks in its own dialog and says so; anything else still
        // gets a native confirmation.
        if (!msg["confirmed"].asBool(false)) {
            const std::wstring prompt =
                L"למחוק את השכבה \"" + layers_[i].name + L"\"?\n\nאי אפשר לבטל את הפעולה.";
            if (HebrewMessageBox(manager_ ? manager_->Hwnd() : nullptr, prompt.c_str(),
                                 MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return;
        }
        layers_.erase(layers_.begin() + i);
        if (positionIndex_ == i)
            RemapPositionIndex(-1);
        else if (positionIndex_ > i)
            RemapPositionIndex(positionIndex_ - 1);
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
        const int pos = positionIndex_;
        if (pos == from)
            RemapPositionIndex(to);
        else if (from < pos && pos <= to)
            RemapPositionIndex(pos - 1);
        else if (to <= pos && pos < from)
            RemapPositionIndex(pos + 1);
        RebuildOverlays();
        SendLayers(selected_);
        return;
    }

    if (cmd == "needPreview") {
        SendPreview(ToWide(msg["path"].asString()), std::clamp(msg["bg"].asInt(0), 0, 100));
        return;
    }

    if (cmd == "sticker") {
        // Only a name from the built-in list; never a path from the page.
        const std::wstring name = ToWide(msg["name"].asString());
        const auto names = StickerNames();
        if (valid && std::find(names.begin(), names.end(), name) != names.end())
            SetLayerImage(i, StickerPath(name));
        return;
    }

    if (cmd == "widget") {
        // The kiosk timer, the one built-in layer that is drawn, not a picture.
        if (!valid || msg["name"].asString() != "timer") return;
        Layer& l = layers_[i];
        if (!IsTimerImage(l.image)) {
            const bool hadPicture = !l.image.empty();
            l.image = kTimerImage;
            l.timerStart = kTimerDefaultStart;
            l.timerRemaining = kTimerDefaultStart;
            l.timerUsed = 0;
            l.timerRunning = true;
            l.timerLocked = false;
            const SIZE bar = timerface::Size(false, 1.0);  // for the previews
            l.width = bar.cx;
            l.height = bar.cy;
            l.rotation = 0;
            if (!hadPicture) {  // its usual place: close to the top-left corner
                l.anchorH = ANCHOR_LEFT;
                l.anchorV = ANCHOR_TOP;
                l.offsetX = 1.0;
                l.offsetY = 1.0;
                if (l.name.rfind(L"שכבה", 0) == 0) l.name = L"טיימר עמדה";
            }
        }
        SyncOverlay(i);
        SaveConfig(settings_, layers_);
        SendPreview(l.image, 0);
        SendLayers(selected_);
        return;
    }

    if (cmd == "timer") {
        if (!valid || !IsTimerImage(layers_[i].image)) return;
        Layer& l = layers_[i];
        const std::string op = msg["op"].asString();
        const int v = std::max(0, msg["value"].asInt(0));
        if (op == "set") {
            l.timerRemaining = v;
        } else if (op == "start") {
            l.timerStart = v;
            l.timerRemaining = v;
            l.timerUsed = 0;
        } else if (op == "reset") {
            l.timerRemaining = l.timerStart;
            l.timerUsed = 0;
        } else if (op == "add") {
            l.timerRemaining = std::max(0.0, l.timerRemaining + msg["value"].asInt(0) * 60.0);
        } else if (op == "running") {
            l.timerRunning = msg["value"].asBool(l.timerRunning);
        } else if (op == "locked") {
            l.timerLocked = msg["value"].asBool(l.timerLocked);
        } else {
            return;
        }
        SyncOverlay(i);
        SaveConfig(settings_, layers_);
        SendLayers(selected_);
        return;
    }

    if (cmd == "setHotkey") {
        SetHotkey(msg["which"].asString() == "cycle", (unsigned)msg["mods"].asInt(0),
                  (unsigned)msg["vk"].asInt(0));
        return;
    }

    if (cmd == "stats") {
        SendStats();
        return;
    }

    if (cmd == "resetStats") {
        AccountUsage();
        for (auto& l : layers_) l.usageSeconds = 0;
        settings_.statsSince = TodayIso();
        SaveConfig(settings_, layers_);
        SendStats();
        return;
    }

    if (cmd == "resetSettings") {
        ResetSettings();
        Toast("ההגדרות חזרו לברירת המחדל. השכבות לא השתנו", false);
        return;
    }

    if (cmd == "resetAll") {
        ResetAll();
        return;
    }

    if (cmd == "undoReset") {
        UndoReset();
        return;
    }

    if (cmd == "nextLayer") {
        ShowNextLayer();
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
        if (!valid) return;
        if (msg["cancel"].asBool(false))
            CancelPositionMode();
        else
            SetPositionMode(i, msg["on"].asBool(true));
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
        // The frame (title bar, the fill behind the page) follows the page's theme.
        if (manager_) manager_->SetTheme(settings_.lightTheme);
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
        // The page saves by itself after every edit and keeps the outcome in
        // its status bar, so a failure is a state there rather than a toast
        // repeated on every keystroke.
        js::Object m;
        m["t"] = "saved";
        m["ok"] = SaveConfig(settings_, layers_);
        Send(js::Value(std::move(m)).dump());
        return;
    }

    if (cmd == "checkUpdate") {
        StartUpdateCheck(true, settings_.autoUpdate);
        return;
    }

    if (cmd == "installUpdate") {
        updateError_.clear();
        StartUpdateCheck(true, true);
        return;
    }

    if (cmd == "openReleases") {
        ShellExecuteW(nullptr, L"open", ToWide(ReleasesPageUrl()).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return;
    }

    if (cmd == "openLink") {
        // A fixed list, not a URL from the page: the page can only name one.
        const std::string which = msg["which"].asString();
        const wchar_t* url = which == "home"     ? L"https://github.com/BeniaBot/floaty"
                           : which == "original" ? L"https://github.com/tsoolgee/overlay-manager-win"
                           : which == "tsoolgee" ? L"https://github.com/tsoolgee"
                           : nullptr;
        if (url) ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    if (cmd == "dismissUpdated") {
        updated_ = UpdateRecord{};
        updateFailedTo_.clear();
        SendUpdateState();
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
    // A built-in sticker is not a path the file dialog could start from.
    if (!IsStickerPath(layers_[index].image))
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
    SetLayerImage(index, file);
}

void App::SetLayerImage(int index, const std::wstring& image) {
    Layer& l = layers_[index];
    const bool hadPicture = !l.image.empty();
    // Still where the previous sticker goes by default (not moved since)?
    // Then the new one may take its own usual place too.
    double prevX = 0, prevY = 0;
    const bool atStickerPlace = StickerDefaultPlace(l.image, prevX, prevY) &&
                                l.anchorH == ANCHOR_LEFT && l.anchorV == ANCHOR_BOTTOM &&
                                std::fabs(l.offsetX - prevX) < 0.05 && std::fabs(l.offsetY - prevY) < 0.05;
    l.image = image;

    // Fit the box to the new image's aspect ratio, keeping the current width.
    int iw = 0, ih = 0;
    const int stickerWidth = StickerDefaultWidth(image);
    if (stickerWidth && ImagePixelSize(l.image, iw, ih) && iw > 0) {
        // A built-in sticker comes at its own size, in proportion; on a layer
        // that had no picture yet it also takes its usual place, the
        // bottom-left corner just above the taskbar.
        l.width = stickerWidth;
        l.height = std::max(16, (int)std::lround((double)stickerWidth * ih / iw));
        double offX = 0, offY = 0;
        if ((!hadPicture || atStickerPlace) && StickerDefaultPlace(image, offX, offY)) {
            l.anchorH = ANCHOR_LEFT;
            l.anchorV = ANCHOR_BOTTOM;
            l.offsetX = offX;
            l.offsetY = offY;
        }
    } else if (ImagePixelSize(l.image, iw, ih) && iw > 0) {
        if (l.lockAspect)
            l.height = std::max(16, (int)std::lround((double)l.width * ih / iw));
        // A layer that never had a picture starts life at the image's own size,
        // clamped so a huge photo does not cover the whole screen. (This used
        // to test for the name "שכבה חדשה", which new layers no longer get:
        // "Add" names them "שכבה 1", "שכבה 2"...)
        if (!hadPicture) {
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
    if (layers_.size() > 1) AppendMenuW(menu, MF_STRING, CMD_NEXT_LAYER, L"השכבה הבאה");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"יציאה");

    POINT pt;
    GetCursorPos(&pt);
    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(hidden_);
    // TPM_LAYOUTRTL: a Hebrew menu, aligned and ordered right to left.
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_LAYOUTRTL, pt.x, pt.y, 0, hidden_,
                   nullptr);
    PostMessageW(hidden_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void App::RegisterHotkey() {
    // MOD_NOREPEAT: holding the keys down toggles once, not on every repeat.
    UnregisterHotKey(hidden_, kHotkeyId);
    hotkeyOk_ = RegisterHotKey(hidden_, kHotkeyId, settings_.hotkeyMods | MOD_NOREPEAT,
                               settings_.hotkeyVk) != FALSE;
    UnregisterHotKey(hidden_, kCycleHotkeyId);
    cycleOk_ = settings_.cycleVk == 0 ||
               RegisterHotKey(hidden_, kCycleHotkeyId, settings_.cycleMods | MOD_NOREPEAT,
                              settings_.cycleVk) != FALSE;
    // Another program already owns a combination. Not fatal: everything else
    // still works, so say so once instead of refusing to start.
    for (const auto& [ok, label] : {std::pair{hotkeyOk_, HotkeyLabel(settings_)},
                                    std::pair{cycleOk_, HotkeyLabel(settings_.cycleMods, settings_.cycleVk)}}) {
        if (ok) continue;
        js::Object m;
        m["t"] = "toast";
        m["text"] = "צירוף המקשים " + label + " תפוס על ידי תוכנה אחרת";
        m["warn"] = true;
        Send(js::Value(std::move(m)).dump());
    }
}

void App::SetHotkey(bool cycle, unsigned mods, unsigned vk) {
    mods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    if (vk > 0xFE) return;
    const int id = cycle ? kCycleHotkeyId : kHotkeyId;
    unsigned& curMods = cycle ? settings_.cycleMods : settings_.hotkeyMods;
    unsigned& curVk = cycle ? settings_.cycleVk : settings_.hotkeyVk;
    bool& ok = cycle ? cycleOk_ : hotkeyOk_;
    auto toast = [this](const std::string& text, bool warn) {
        js::Object t;
        t["t"] = "toast";
        t["text"] = text;
        t["warn"] = warn;
        Send(js::Value(std::move(t)).dump());
    };

    // Hiding everything has no "off": without it there is no way to get the
    // layers out of a screenshot. Only the next-layer key can be turned off.
    if (vk == 0 && !cycle) return;
    const unsigned otherMods = cycle ? settings_.hotkeyMods : settings_.cycleMods;
    const unsigned otherVk = cycle ? settings_.hotkeyVk : settings_.cycleVk;
    if (vk && vk == otherVk && mods == otherMods) {
        toast("הצירוף " + HotkeyLabel(mods, vk) + " כבר משמש לקיצור השני", true);
        SendSettings();
        return;
    }

    UnregisterHotKey(hidden_, id);
    if (vk && !RegisterHotKey(hidden_, id, mods | MOD_NOREPEAT, vk)) {
        // Taken by another program: keep the one that worked.
        if (curVk) ok = RegisterHotKey(hidden_, id, curMods | MOD_NOREPEAT, curVk) != FALSE;
        toast("הצירוף " + HotkeyLabel(mods, vk) + " תפוס על ידי תוכנה אחרת. הקיצור לא השתנה", true);
        SendSettings();
        return;
    }
    curMods = mods;
    curVk = vk;
    ok = true;
    SaveConfig(settings_, layers_);
    SendSettings();
    toast(vk ? "הקיצור עודכן ל-" + HotkeyLabel(mods, vk) : std::string("הקיצור לשכבה הבאה כובה"), false);
}

void App::Toast(const std::string& text, bool warn, const char* action, const char* actionLabel) {
    js::Object t;
    t["t"] = "toast";
    t["text"] = text;
    t["warn"] = warn;
    if (action) {
        // A button in the toast that sends this command back (e.g. undo).
        t["action"] = action;
        t["actionLabel"] = actionLabel ? actionLabel : "";
    }
    Send(js::Value(std::move(t)).dump());
}

void App::SendStats() {
    AccountUsage();
    js::Object m;
    m["t"] = "stats";
    m["since"] = settings_.statsSince;
    js::Array items;
    for (const auto& l : layers_) {
        js::Object o;
        o["name"] = ToUtf8(l.name);
        o["image"] = ToUtf8(l.image);
        o["seconds"] = (double)(long long)l.usageSeconds;
        items.push_back(js::Value(std::move(o)));
    }
    m["items"] = js::Value(std::move(items));
    Send(js::Value(std::move(m)).dump());
}

void App::ResetSettings() {
    // Back to what a first run starts with. The layers are left as they are;
    // autostart is a registry entry, so it is removed there too.
    if (IsAutoStartEnabled()) SetAutoStartEnabled(false);
    const std::string since = settings_.statsSince;
    settings_ = DefaultSettings();
    settings_.statsSince = since;
    RegisterHotkey();
    if (!trayAdded_) CreateTrayIcon();
    if (manager_) manager_->SetTheme(settings_.lightTheme);
    SaveConfig(settings_, layers_);
    SendSettings();
}

void App::ResetAll() {
    // Everything goes, as on a fresh install - but a copy of the configuration
    // is kept first, and the toast offers to bring it back.
    const std::wstring backup = ConfigDir() + L"\\config-before-reset.ini";
    AccountUsage();
    SaveConfig(settings_, layers_);
    CopyFileW(ConfigPath().c_str(), backup.c_str(), FALSE);

    if (positionIndex_ >= 0) SetPositionMode(positionIndex_, false);
    layers_.clear();
    selected_ = -1;
    allHidden_ = false;
    ResetSettings();
    settings_.statsSince = TodayIso();
    SaveConfig(settings_, layers_);
    RebuildOverlays();
    UpdateTrayIcon();
    SendInit();
    Toast("התוכנה אופסה: כל השכבות וההגדרות נמחקו", true, "undoReset", "להחזיר את מה שהיה");
}

void App::UndoReset() {
    const std::wstring backup = ConfigDir() + L"\\config-before-reset.ini";
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!CopyFileW(backup.c_str(), ConfigPath().c_str(), FALSE)) {
        Toast("לא ניתן היה לשחזר את ההגדרות הקודמות", true);
        return;
    }
    DeleteFileW(backup.c_str());
    settings_ = DefaultSettings();
    layers_.clear();
    LoadConfig(settings_, layers_);
    if (settings_.statsSince.empty()) settings_.statsSince = TodayIso();
    // The saved file remembers whether autostart was on; the registry entry
    // itself was removed by the reset, so put it back to match.
    if (settings_.autoStart) SetAutoStartEnabled(true);
    settings_.autoStart = IsAutoStartEnabled();
    RegisterHotkey();
    if (settings_.showTrayIcon) CreateTrayIcon(); else RemoveTrayIcon();
    if (manager_) manager_->SetTheme(settings_.lightTheme);
    selected_ = layers_.empty() ? -1 : 0;
    RebuildOverlays();
    SendInit();
    Toast("השכבות וההגדרות חזרו", false);
}

void App::ShowNextLayer() {
    AccountUsage();
    // Only layers with a picture take part; a layer without one would show
    // nothing, which looks like the key did not work.
    std::vector<int> pics;
    for (int i = 0; i < (int)layers_.size(); ++i)
        if (!layers_[i].image.empty()) pics.push_back(i);
    if (pics.empty()) return;

    // From exactly one shown layer, move to the next; from any other state
    // (none shown, several shown), start at the first.
    int shown = -1, count = 0;
    for (int i : pics)
        if (layers_[i].enabled) { shown = i; ++count; }
    int next = pics.front();
    if (count == 1) {
        const auto it = std::find(pics.begin(), pics.end(), shown);
        next = (it + 1 == pics.end()) ? pics.front() : *(it + 1);
    }

    for (int i = 0; i < (int)layers_.size(); ++i) {
        const bool want = i == next;
        if (layers_[i].enabled == want) continue;
        layers_[i].enabled = want;
        SyncOverlay(i);
    }
    if (allHidden_) SetAllHidden(false);
    SendLayers(selected_);
    SaveConfig(settings_, layers_);
}

// --- updates ---------------------------------------------------------------

void App::StartUpdateCheck(bool manual, bool install) {
    if (updateBusy_) return;
    if (!updateReady_.empty()) {
        // Already downloaded; checking again would only fetch the same file.
        if (manual) SendUpdateState();
        return;
    }
    updateBusy_ = true;
    if (manual) SendUpdateState();

    const HWND target = hidden_;
    std::thread([target, manual, install] {
        auto* r = new UpdateResult;
        r->manual = manual;
        r->info = CheckForUpdate();
        // With automatic install off, a newer release is only announced; it is
        // installed when the user presses "Update now".
        if (r->info.ok && r->info.newer && install) {
            r->installError = DownloadAndInstall(r->info);
            r->installed = r->installError.empty();
        }
        if (!PostMessageW(target, WM_UPDATE_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void App::OnUpdateFinished(UpdateResult* result) {
    std::unique_ptr<UpdateResult> r(result);
    updateBusy_ = false;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long ticks = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    lastCheck_.atMs = (long long)(ticks / 10000ULL - 11644473600000ULL);  // Unix milliseconds
    lastCheck_.ok = r->info.ok;
    lastCheck_.latest = r->info.version;
    lastCheck_.published = r->info.publishedAt;
    lastCheck_.error = r->info.error;

    // "26.11.2026" from "2026-11-26", for the messages below.
    std::string published;
    if (r->info.publishedAt.size() == 10)
        published = r->info.publishedAt.substr(8, 2) + "." + r->info.publishedAt.substr(5, 2) + "." +
                    r->info.publishedAt.substr(0, 4);

    std::string text;
    bool warn = false;
    if (r->installed) {
        updateReady_ = r->info.version;
        updateNotes_ = r->info.notes;
        updateAvailable_.clear();
        updateError_.clear();
        // The next start reads this to say "updated to X" with the notes, or
        // that the swap did not take.
        WriteUpdateRecord(r->info);
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
        // Kept as state: the page shows it with a manual-download button until
        // an install succeeds, since a toast is gone before anyone reads it.
        updateError_ = "העדכון לגרסה " + r->info.version + " לא הותקן: " + r->installError;
        updateAvailable_ = r->info.version;
        updateNotes_ = r->info.notes;
    } else if (!r->info.error.empty()) {
        text = r->info.error;
        warn = true;
    } else if (r->info.newer) {
        updateAvailable_ = r->info.version;
        updateNotes_ = r->info.notes;
        text = "נמצאה גרסה " + r->info.version + (published.empty() ? "" : ", שפורסמה ב-" + published) +
               ". הגרסה שלך: " + AppVersion();
    } else {
        updateAvailable_.clear();
        text = std::string("יש לך את הגרסה העדכנית ביותר, ") + AppVersion() +
               (published.empty() ? "" : " (פורסמה ב-" + published + ")");
    }

    SendUpdateState();
    // A background check stays quiet; what it found is in the update state.
    if (!text.empty() && r->manual) {
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
    AccountUsage();
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
                case CMD_NEXT_LAYER: ShowNextLayer(); return 0;
                default: break;
            }
            break;

        case WM_UPDATE_DONE:
            OnUpdateFinished(reinterpret_cast<UpdateResult*>(lp));
            return 0;

        case WM_TIMER:
            if (wp == kUpdateTimer) {
                SetTimer(hidden_, kUpdateTimer, kCheckEveryMs, nullptr);
                // Always look; install only when automatic updates are on.
                StartUpdateCheck(false, settings_.autoUpdate);
                return 0;
            }
            if (wp == kUsageTimer) {
                AccountUsage();
                // A running kiosk timer is saved every tick (15 s), as the
                // original did, so a power cut costs it seconds, not minutes.
                const bool timerRunning = std::any_of(layers_.begin(), layers_.end(), [](const Layer& l) {
                    return IsTimerImage(l.image) && l.timerRunning && l.timerRemaining > 0;
                });
                if (++usageTicks_ >= kUsageSaveEveryTicks || timerRunning) {
                    usageTicks_ = 0;
                    SaveConfig(settings_, layers_);
                }
                return 0;
            }
            break;

        case WM_HOTKEY:
            if (wp == kHotkeyId) SetAllHidden(!allHidden_);
            else if (wp == kCycleHotkeyId) ShowNextLayer();
            return 0;

        case WM_DISPLAYCHANGE: {
            // A monitor was added, removed or rearranged. Re-read the layout and
            // move every overlay onto its target screen's new rectangle.
            // A layer whose monitor is gone keeps its index: MonitorAt() shows
            // it on the primary screen meanwhile, and it returns to its own
            // screen when that is plugged back in, instead of the layout being
            // rewritten (and saved) the moment a laptop leaves its dock.
            RefreshMonitors();
            for (auto& o : overlays_) o->Reposition();
            SendInit();
            return 0;
        }

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            // Windows is shutting down; there is no second chance to write.
            if (wp) SaveConfig(settings_, layers_);
            return 0;

        case WM_DESTROY:
            if (!quitting_) PostQuitMessage(0);
            return 0;

        default:
            if (msg == WM_TASKBAR_CREATED && msg != 0) {
                // Explorer restarted and forgot our icon; put it back. Remove
                // first, so a broadcast that arrives while the icon is still
                // there does not fail the add and leave it orphaned.
                if (trayAdded_) RemoveTrayIcon();
                if (settings_.showTrayIcon && CreateTrayIcon()) UpdateTrayIcon();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hidden_, msg, wp, lp);
}
