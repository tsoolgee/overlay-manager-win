#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>

#include "json.h"
#include "model.h"
#include "util.h"
#include "version.h"

namespace {

// The repository whose releases this build updates from.
const char* kRepo = "BeniaBot/floaty";
const char* kExeAsset = "Floaty.exe";
const char* kHashAsset = "Floaty.exe.sha256";

// Set only by tests: a stand-in for GitHub's "latest release" endpoint.
std::wstring TestApiUrl() {
    wchar_t buf[1024] = {0};
    const DWORD n = GetEnvironmentVariableW(L"FLOATY_UPDATE_API", buf, (DWORD)std::size(buf));
    return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
}

std::string LatestApiUrl() {
    const std::wstring test = TestApiUrl();
    if (!test.empty()) return ToUtf8(test);
    return std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
}

// A release asset is ~420 KB; anything far past that is not ours.
const size_t kMaxDownload = 64u * 1024 * 1024;

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

HINTERNET OpenSession() {
    const std::wstring agent = L"Floaty/" + ToWide(AppVersion());
    // Automatic proxy (Windows 8.1+) follows the system's proxy settings;
    // older systems reject the flag, so fall back to the registry default.
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    return s;
}

// GET over HTTPS into memory. Redirects (GitHub sends asset downloads to its
// CDN) are followed by WinHTTP itself. Returns the HTTP status, 0 on failure.
int HttpGet(const std::string& url, std::string& body, const wchar_t* accept) {
    body.clear();
    const std::wstring wurl = ToWide(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    std::vector<wchar_t> path(wurl.size() + 1);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path.data();
    uc.dwUrlPathLength = (DWORD)path.size();
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return 0;
    // Never fetch code over plain HTTP — except from this machine itself while
    // a test has pointed the updater at a local stand-in for GitHub.
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const bool local = _wcsicmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0;
    if (!https && !(local && !TestApiUrl().empty())) return 0;

    Handle session(OpenSession());
    if (!session) return 0;
    Handle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn) return 0;
    Handle req(WinHttpOpenRequest(conn.h, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return 0;

    std::wstring headers;
    if (accept) headers = std::wstring(L"Accept: ") + accept + L"\r\n";
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return 0;

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return 0;
        if (avail == 0) break;
        const size_t at = body.size();
        if (at + avail > kMaxDownload) return 0;
        body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, &body[at], avail, &read)) return 0;
        body.resize(at + read);
    }
    return (int)status;
}

std::string Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {0};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    std::string hex;
    char b[3];
    for (unsigned char c : digest) {
        snprintf(b, sizeof(b), "%02X", c);
        hex += b;
    }
    return hex;
}

// First run of 64 hex digits in the text, upper-cased. Both the .sha256 asset
// and the "SHA-256: …" line in the release notes carry the hash this way.
std::string FindSha256(const std::string& text) {
    size_t run = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (std::isxdigit((unsigned char)text[i])) {
            if (++run == 64 && (i + 1 == text.size() || !std::isxdigit((unsigned char)text[i + 1]))) {
                std::string h = text.substr(i + 1 - 64, 64);
                for (auto& c : h) c = (char)std::toupper((unsigned char)c);
                return h;
            }
        } else {
            run = 0;
        }
    }
    return {};
}

// "v3.10.2" → {3,10,2}. Missing parts count as 0.
std::vector<int> ParseVersion(const std::string& s) {
    std::vector<int> parts;
    int cur = -1;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        } else if (c == '.') {
            parts.push_back(cur < 0 ? 0 : cur);
            cur = -1;
        } else if (cur >= 0) {
            break;  // "3.2.0-beta": stop at the suffix
        }
    }
    if (cur >= 0) parts.push_back(cur);
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

bool IsNewer(const std::string& latest, const std::string& current) {
    const auto a = ParseVersion(latest), b = ParseVersion(current);
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return a.size() > b.size();
}

// Release notes are Markdown; the page shows them as plain lines. Emphasis and
// heading marks go, list items become bullets, and the hash line — meant for
// people verifying a download by hand — is left out.
std::string PlainNotes(const std::string& md) {
    std::string out, line;
    auto flush = [&] {
        std::string s = line;
        line.clear();
        for (const char* mark : {"**", "__", "`"}) {
            for (size_t p; (p = s.find(mark)) != std::string::npos;) s.erase(p, strlen(mark));
        }
        size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) { if (!out.empty() && out.back() != '\n') out += '\n'; return; }
        s = s.substr(a);
        while (!s.empty() && s[0] == '#') s.erase(0, 1);
        if (s.size() > 1 && (s[0] == '-' || s[0] == '*') && s[1] == ' ') s = "\xE2\x80\xA2 " + s.substr(2);
        if (!FindSha256(s).empty()) return;
        a = s.find_first_not_of(' ');
        if (a == std::string::npos) return;
        out += s.substr(a);
        out += '\n';
    };
    for (char c : md) {
        if (c == '\n') flush(); else line += c;
    }
    flush();
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    if (out.size() > 2000) {
        // Cut on a character boundary: UTF-8 continuation bytes are 10xxxxxx.
        size_t cut = 2000;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut) + "\xE2\x80\xA6";
    }
    return out;
}

std::wstring RecordPath() { return ConfigDir() + L"\\update.json"; }

bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

}  // namespace

const char* AppVersion() { return FLOATY_VERSION; }

std::string ReleasesPageUrl() { return std::string("https://github.com/") + kRepo + "/releases/latest"; }

UpdateInfo CheckForUpdate() {
    UpdateInfo info;
    std::string body;
    const int status = HttpGet(LatestApiUrl(), body, L"application/vnd.github+json");
    if (status != 200) {
        // GitHub allows 60 unauthenticated checks an hour per address; on a
        // shared network (an office, a school) that runs out, and it answers
        // 403 or 429. Say what it is rather than quote the number.
        if (status == 0)
            info.error = "אין חיבור לאינטרנט, או שהגישה לגיטהאב חסומה ברשת הזו";
        else if (status == 403 || status == 429)
            info.error = "גיטהאב הגביל זמנית את מספר הבדיקות מהרשת הזו. אפשר לנסות שוב בעוד שעה";
        else if (status == 404)
            info.error = "עדיין לא פורסמה גרסה להורדה";
        else
            info.error = "שרת העדכונים החזיר שגיאה " + std::to_string(status);
        return info;
    }

    const js::Value rel = js::Value::parse(body);
    const std::string tag = rel["tag_name"].asString();
    if (tag.empty()) {
        info.error = "תשובה לא צפויה משרת העדכונים";
        return info;
    }

    info.ok = true;
    info.version = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    info.newer = IsNewer(info.version, AppVersion());

    std::string hashUrl;
    for (const auto& a : rel["assets"].asArray()) {
        const std::string& name = a["name"].asString();
        if (name == kExeAsset) info.downloadUrl = a["browser_download_url"].asString();
        if (name == kHashAsset) hashUrl = a["browser_download_url"].asString();
    }

    // Prefer the .sha256 asset; fall back to the hash in the release notes.
    if (!hashUrl.empty()) {
        std::string h;
        if (HttpGet(hashUrl, h, nullptr) == 200) info.sha256 = FindSha256(h);
    }
    if (info.sha256.empty()) info.sha256 = FindSha256(rel["body"].asString());
    info.notes = PlainNotes(rel["body"].asString());

    if (info.newer && info.downloadUrl.empty()) {
        info.newer = false;
        info.error = "בגרסה החדשה אין קובץ להורדה";
    }
    return info;
}

std::string DownloadAndInstall(const UpdateInfo& info) {
    // Without a published hash there is no way to tell a truncated or
    // tampered download from the real thing, so do not install it.
    if (info.sha256.size() != 64) return "לגרסה החדשה אין חתימת SHA-256 לאימות";

    std::string data;
    if (HttpGet(info.downloadUrl, data, L"application/octet-stream") != 200 || data.empty())
        return "ההורדה נכשלה";
    if (data.size() < 2 || data[0] != 'M' || data[1] != 'Z')
        return "הקובץ שהורד אינו תוכנה תקינה";
    if (Sha256Hex(data) != info.sha256)
        return "הקובץ שהורד לא עבר את בדיקת ה-SHA-256";

    const std::wstring exe = ExePath();
    const std::wstring fresh = exe + L".new";
    const std::wstring old = exe + L".old";

    if (!WriteAll(fresh, data))
        return "אין הרשאה לעדכן את התוכנה בתיקייה שבה היא נמצאת. אפשר להוריד את הגרסה החדשה ידנית";

    // A leftover from an earlier update would block the rename below.
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(fresh.c_str());
        return "לא ניתן היה להחליף את קובץ התוכנה";
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Put the running copy back so the program still starts next time.
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(fresh.c_str());
        return "לא ניתן היה להחליף את קובץ התוכנה";
    }
    return {};
}

bool RelaunchUpdated(bool minimized) {
    std::wstring cmd = L"\"" + ExePath() + L"\" /updated";
    if (minimized) cmd += L" /minimized";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void WriteUpdateRecord(const UpdateInfo& info) {
    js::Object o;
    o["from"] = AppVersion();
    o["to"] = info.version;
    o["notes"] = info.notes;
    o["exe"] = ToUtf8(ExePath());
    WriteAll(RecordPath(), js::Value(std::move(o)).dump());
}

UpdateRecord TakeUpdateRecord() {
    UpdateRecord r;
    const std::wstring path = RecordPath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return r;
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (text.size() < 256 * 1024 && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got)
        text.append(buf, got);
    CloseHandle(f);
    DeleteFileW(path.c_str());  // read once, whatever it says
    const js::Value v = js::Value::parse(text);
    r.to = v["to"].asString();
    r.from = v["from"].asString();
    r.notes = v["notes"].asString();
    r.exe = v["exe"].asString();
    r.present = !r.to.empty();
    return r;
}

void CleanupAfterUpdate() {
    const std::wstring old = ExePath() + L".old";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::thread([old] {
        // The previous process may take a moment to exit and unlock its image.
        for (int i = 0; i < 60; ++i) {
            if (DeleteFileW(old.c_str()) ||
                GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES)
                return;
            Sleep(1000);
        }
    }).detach();
}
