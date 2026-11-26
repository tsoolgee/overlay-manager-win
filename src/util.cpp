#include "util.h"

#include <vector>

namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"Floaty";
}  // namespace

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring ExePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);  // long path; try again with more room
    }
}

bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    const bool present =
        RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &size) ==
        ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

std::wstring AutoStartTarget() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[2048] = {0};
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    const bool ok = RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<BYTE*>(buf),
                                     &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    if (!ok) return {};
    // Written as "<path>" /minimized: the path is what is between the quotes.
    std::wstring cmd = buf;
    if (cmd.size() > 1 && cmd[0] == L'"') {
        const size_t end = cmd.find(L'"', 1);
        if (end != std::wstring::npos) return cmd.substr(1, end - 1);
    }
    return {};
}

void SetAutoStartEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS)
        return;

    if (enabled) {
        // Quote the path (it usually contains spaces) and start minimised, so a
        // login does not throw the manager window in the user's face.
        std::wstring cmd = L"\"" + ExePath() + L"\" /minimized";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}
