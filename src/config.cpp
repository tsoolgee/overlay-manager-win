#include "model.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

const wchar_t* kGeneral = L"General";

std::wstring Section(int i) {
    wchar_t buf[32];
    swprintf(buf, 32, L"Overlay_%d", i);
    return buf;
}

std::wstring GetStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def,
                    const std::wstring& path) {
    // GetPrivateProfileString truncates silently, so grow the buffer until the
    // value fits rather than clipping a long image path.
    std::vector<wchar_t> buf(512);
    for (;;) {
        const DWORD n = GetPrivateProfileStringW(sec, key, def, buf.data(),
                                                 (DWORD)buf.size(), path.c_str());
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

int GetInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& path) {
    return (int)GetPrivateProfileIntW(sec, key, def, path.c_str());
}

double GetDouble(const wchar_t* sec, const wchar_t* key, double def,
                 const std::wstring& path) {
    const std::wstring s = GetStr(sec, key, L"", path);
    if (s.empty()) return def;
    return _wtof(s.c_str());
}

void PutStr(std::wstring& out, const wchar_t* key, const std::wstring& value) {
    out += key;
    out += L'=';
    out += value;
    out += L"\r\n";
}

void PutInt(std::wstring& out, const wchar_t* key, long long value) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%lld", value);
    PutStr(out, key, buf);
}

void PutDouble(std::wstring& out, const wchar_t* key, double value) {
    wchar_t buf[40];
    swprintf(buf, 40, L"%.2f", value);
    PutStr(out, key, buf);
}

}  // namespace

std::wstring ConfigDir() {
    wchar_t appdata[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        return L".";
    std::wstring dir = appdata;
    dir += L"\\OverlayManager";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\config.ini"; }

bool LoadConfig(Settings& settings, std::vector<Layer>& layers) {
    const std::wstring path = ConfigPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    settings.autoStart      = GetInt(kGeneral, L"AutoStart", 0, path) != 0;
    settings.minimizeToTray = GetInt(kGeneral, L"MinimizeToTray", 1, path) != 0;
    settings.showTrayIcon   = GetInt(kGeneral, L"ShowTrayIcon", 1, path) != 0;
    settings.lightTheme     = GetInt(kGeneral, L"LightTheme", 0, path) != 0;
    settings.autoUpdate     = GetInt(kGeneral, L"AutoUpdate", 1, path) != 0;
    settings.hotkeyMods     = (unsigned)GetInt(kGeneral, L"HotkeyMods",
                                               MOD_CONTROL | MOD_ALT, path);
    settings.hotkeyVk       = (unsigned)GetInt(kGeneral, L"HotkeyVk", 'H', path);

    const int count = GetInt(kGeneral, L"Count", 0, path);
    layers.clear();
    for (int i = 0; i < count; ++i) {
        const std::wstring sec = Section(i);
        const wchar_t* s = sec.c_str();

        // Keys absent from an older config.ini fall back to the struct's own
        // defaults, which is what keeps a version 3 configuration loading.
        Layer l;
        l.name         = GetStr(s, L"Name", L"שכבה", path);
        l.image        = GetStr(s, L"Image", L"", path);
        l.enabled      = GetInt(s, L"Enabled", 1, path) != 0;
        l.width        = GetInt(s, L"Width", 200, path);
        l.height       = GetInt(s, L"Height", 200, path);
        l.lockAspect   = GetInt(s, L"LockAspect", 1, path) != 0;
        l.monitor      = GetInt(s, L"Monitor", 0, path);
        l.anchorH      = GetInt(s, L"AnchorH", ANCHOR_RIGHT, path);
        l.anchorV      = GetInt(s, L"AnchorV", ANCHOR_BOTTOM, path);
        l.offsetX      = GetDouble(s, L"OffsetX", 2.0, path);
        l.offsetY      = GetDouble(s, L"OffsetY", 2.0, path);
        l.opacity      = GetInt(s, L"Opacity", 255, path);
        l.useTint      = GetInt(s, L"UseTint", 0, path) != 0;
        l.tintR        = GetInt(s, L"TintR", 255, path);
        l.tintG        = GetInt(s, L"TintG", 255, path);
        l.tintB        = GetInt(s, L"TintB", 255, path);
        l.rotation     = GetDouble(s, L"Rotation", 0.0, path);
        l.clickThrough = GetInt(s, L"ClickThrough", 1, path) != 0;
        l.grayscale    = GetInt(s, L"Grayscale", 0, path) != 0;
        l.removeBg     = GetInt(s, L"RemoveBg", 0, path) != 0;
        l.bgTolerance  = GetInt(s, L"BgTolerance", 25, path);
        layers.push_back(std::move(l));
    }
    return true;
}

bool SaveConfig(const Settings& settings, const std::vector<Layer>& layers) {
    // The original wrote key by key with WritePrivateProfileString, so a crash
    // mid-save could leave a half-written file, and deleting a layer left the
    // now-surplus [Overlay_N] section behind. Build the whole file, then swap
    // it in with one atomic rename.
    std::wstring text = L"[General]\r\n";
    PutInt(text, L"Count", (long long)layers.size());
    PutInt(text, L"AutoStart", settings.autoStart ? 1 : 0);
    PutInt(text, L"MinimizeToTray", settings.minimizeToTray ? 1 : 0);
    PutInt(text, L"ShowTrayIcon", settings.showTrayIcon ? 1 : 0);
    PutInt(text, L"LightTheme", settings.lightTheme ? 1 : 0);
    PutInt(text, L"AutoUpdate", settings.autoUpdate ? 1 : 0);
    PutInt(text, L"HotkeyMods", settings.hotkeyMods);
    PutInt(text, L"HotkeyVk", settings.hotkeyVk);

    for (size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];
        text += L"\r\n[";
        text += Section((int)i);
        text += L"]\r\n";
        PutStr(text, L"Name", l.name);
        PutStr(text, L"Image", l.image);
        PutInt(text, L"Enabled", l.enabled ? 1 : 0);
        PutInt(text, L"Width", l.width);
        PutInt(text, L"Height", l.height);
        PutInt(text, L"LockAspect", l.lockAspect ? 1 : 0);
        PutInt(text, L"Monitor", l.monitor);
        PutInt(text, L"AnchorH", l.anchorH);
        PutInt(text, L"AnchorV", l.anchorV);
        PutDouble(text, L"OffsetX", l.offsetX);
        PutDouble(text, L"OffsetY", l.offsetY);
        PutInt(text, L"Opacity", l.opacity);
        PutInt(text, L"UseTint", l.useTint ? 1 : 0);
        PutInt(text, L"TintR", l.tintR);
        PutInt(text, L"TintG", l.tintG);
        PutInt(text, L"TintB", l.tintB);
        PutDouble(text, L"Rotation", l.rotation);
        PutInt(text, L"ClickThrough", l.clickThrough ? 1 : 0);
        PutInt(text, L"Grayscale", l.grayscale ? 1 : 0);
        PutInt(text, L"RemoveBg", l.removeBg ? 1 : 0);
        PutInt(text, L"BgTolerance", l.bgTolerance);
    }

    const std::wstring path = ConfigPath();
    const std::wstring temp = path + L".tmp";

    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    // UTF-16 LE with a BOM, so the profile API reads it back as Unicode and
    // Hebrew layer names survive the round trip.
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof bom, &written, nullptr) != FALSE;
    ok = ok && WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)),
                         &written, nullptr) != FALSE;
    ok = ok && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }

    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}
