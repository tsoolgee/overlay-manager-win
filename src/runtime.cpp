#include "runtime.h"

#include <aclapi.h>
#include <commctrl.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <WebView2.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "resource.h"

namespace {

// Floaty-offline.exe is Floaty.exe + the runtime's .cab + this trailer, so the
// two builds share one program and Floaty.exe stays small for updates.
const char kMagic[16] = "FloatyRuntime01";
struct Trailer {
    char magic[16];
    uint64_t size;  // bytes of .cab right before the trailer
};
static_assert(sizeof(Trailer) == 24, "the trailer is written by tools/make-offline.ps1");

const wchar_t* kTitle = L"Floaty";
const wchar_t* kReleases = L"https://github.com/tsoolgee/overlay-manager-win/releases/latest";
// Microsoft's Evergreen bootstrapper: a small download that installs the runtime.
const wchar_t* kBootstrapper = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";

std::wstring RuntimeRoot() {
    // Local, not Roaming: a few hundred megabytes have no business following a
    // domain profile around.
    wchar_t local[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return L"";
    return std::wstring(local) + L"\\Floaty\\WebView2Runtime";
}

bool IsRuntime(const wchar_t* folder) {
    LPWSTR version = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(folder, &version);
    const bool ok = SUCCEEDED(hr) && version != nullptr;
    if (version) CoTaskMemFree(version);
    return ok;
}

// The first subfolder of `dir` that holds a runtime, or empty.
std::wstring RuntimeInside(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring sub = dir + L"\\" + fd.cFileName;
        if (IsRuntime(sub.c_str())) { found = sub; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::wstring SelfPath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return L"";
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

HANDLE OpenForRead(const std::wstring& path) {
    // Share delete too: the updater renames the running executable.
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Where the bundled .cab sits inside this executable, if it carries one.
bool FindPayload(const std::wstring& exe, uint64_t& offset, uint64_t& size) {
    HANDLE f = OpenForRead(exe);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER total{};
    if (GetFileSizeEx(f, &total) && total.QuadPart > (LONGLONG)sizeof(Trailer)) {
        LARGE_INTEGER at;
        at.QuadPart = total.QuadPart - (LONGLONG)sizeof(Trailer);
        Trailer t{};
        DWORD got = 0;
        if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN) &&
            ReadFile(f, &t, sizeof t, &got, nullptr) && got == sizeof t &&
            std::memcmp(t.magic, kMagic, sizeof kMagic) == 0 && t.size > 0 &&
            t.size <= (uint64_t)at.QuadPart) {
            offset = (uint64_t)at.QuadPart - t.size;
            size = t.size;
            ok = true;
        }
    }
    CloseHandle(f);
    return ok;
}

void DeleteTree(const std::wstring& dir) {
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring from = dir;
    from.push_back(L'\0');  // SHFileOperation takes a double-NUL-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// The runtime's sandboxed processes run as app containers. Microsoft's Fixed
// Version instructions: give "ALL APPLICATION PACKAGES" and "ALL RESTRICTED
// APPLICATION PACKAGES" read and execute on the runtime folder, or the page
// renderer cannot load its own files from under the user's profile.
bool GrantAppContainers(const std::wstring& dir) {
    PSID sids[2] = {nullptr, nullptr};
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sids[0]) ||
        !ConvertStringSidToSidW(L"S-1-15-2-2", &sids[1])) {
        if (sids[0]) LocalFree(sids[0]);
        return false;
    }
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD r = GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, &current, nullptr, &sd);
    PACL updated = nullptr;
    if (r == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea[2] = {};
        for (int i = 0; i < 2; ++i) {
            ea[i].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
            ea[i].grfAccessMode = GRANT_ACCESS;
            ea[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
            ea[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
            ea[i].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
            ea[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
        }
        r = SetEntriesInAclW(2, ea, current, &updated);
    }
    if (r == ERROR_SUCCESS)
        r = SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    if (updated) LocalFree(updated);
    if (sd) LocalFree(sd);
    LocalFree(sids[0]);
    LocalFree(sids[1]);
    return r == ERROR_SUCCESS;
}

std::wstring LastError(const wchar_t* what) {
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring out = what;
    if (text) {
        out += L": ";
        out += text;
        LocalFree(text);
        while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r')) out.pop_back();
    }
    return out;
}

struct UnpackJob {
    std::wstring exe, root;
    uint64_t offset = 0, size = 0;
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    std::mutex lock;
    HANDLE process = nullptr;  // expand.exe while it runs
    std::wstring result, error;
};

void Unpack(UnpackJob& job) {
    const std::wstring staging = job.root + L"\\unpacking";
    const std::wstring cab = job.root + L"\\runtime.cab";

    auto fail = [&](const std::wstring& why) {
        job.error = why;
        DeleteFileW(cab.c_str());
        DeleteTree(staging);
    };

    SHCreateDirectoryExW(nullptr, job.root.c_str(), nullptr);
    // Set on the root before anything is written, so every file inherits it.
    if (!GrantAppContainers(job.root)) return fail(LastError(L"הרשאות לתיקיית הרכיב"));
    DeleteTree(staging);  // what an interrupted earlier attempt left
    if (!CreateDirectoryW(staging.c_str(), nullptr)) return fail(LastError(L"יצירת תיקייה"));

    // 1. Copy the .cab out of this executable.
    {
        HANDLE in = OpenForRead(job.exe);
        if (in == INVALID_HANDLE_VALUE) return fail(LastError(L"קריאת Floaty"));
        HANDLE out = CreateFileW(cab.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            CloseHandle(in);
            return fail(LastError(L"כתיבת הרכיב"));
        }
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)job.offset;
        bool ok = SetFilePointerEx(in, at, nullptr, FILE_BEGIN) != FALSE;
        std::vector<char> buf(4 << 20);
        uint64_t left = job.size;
        while (ok && left > 0 && !job.cancel) {
            const DWORD want = (DWORD)std::min<uint64_t>(buf.size(), left);
            DWORD got = 0, put = 0;
            ok = ReadFile(in, buf.data(), want, &got, nullptr) && got == want &&
                 WriteFile(out, buf.data(), got, &put, nullptr) && put == got;
            left -= got;
        }
        const std::wstring why = ok ? L"" : LastError(L"כתיבת הרכיב");
        CloseHandle(in);
        CloseHandle(out);
        if (job.cancel) return fail(L"בוטל");
        if (!ok) return fail(why);
    }

    // 2. Unpack it with expand.exe, which every edition of Windows has, the
    //    way Microsoft's instructions for the Fixed Version runtime do.
    {
        wchar_t sys[MAX_PATH] = {0};
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(sys) + L"\\expand.exe\" \"" + cab + L"\" -F:* \"" +
                           staging + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, sys, &si, &pi))
            return fail(LastError(L"הפעלת expand"));
        CloseHandle(pi.hThread);
        {
            std::lock_guard<std::mutex> g(job.lock);
            job.process = pi.hProcess;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        {
            std::lock_guard<std::mutex> g(job.lock);
            job.process = nullptr;
        }
        CloseHandle(pi.hProcess);
        DeleteFileW(cab.c_str());
        if (job.cancel) return fail(L"בוטל");
        if (code != 0) {
            wchar_t msg[64];
            swprintf(msg, 64, L"expand החזיר %lu", code);
            return fail(msg);
        }
    }

    // 3. Move the finished folder into place: only a complete runtime is ever
    //    under the root, so an interrupted unpack cannot be mistaken for one.
    std::wstring inner = RuntimeInside(staging);
    if (inner.empty() && IsRuntime(staging.c_str())) inner = staging;  // a flat .cab
    if (inner.empty()) return fail(L"הרכיב לא נמצא בתוך החבילה");
    std::wstring name = inner.substr(inner.find_last_of(L'\\') + 1);
    if (inner == staging) name = L"runtime";
    const std::wstring target = job.root + L"\\" + name;
    DeleteTree(target);
    if (!MoveFileW(inner.c_str(), target.c_str())) return fail(LastError(L"העברת הרכיב"));
    DeleteTree(staging);
    job.result = target;
}

HRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR ref) {
    auto* job = reinterpret_cast<UnpackJob*>(ref);
    switch (msg) {
        case TDN_CREATED:
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
            break;
        case TDN_TIMER:
            // The worker is done: close the dialog through its only button,
            // which is told apart from a real cancel by `done`.
            if (job->done) SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
            break;
        case TDN_BUTTON_CLICKED:
            if (wp == IDCANCEL && !job->done) {
                job->cancel = true;
                std::lock_guard<std::mutex> g(job->lock);
                if (job->process) TerminateProcess(job->process, 1);
            }
            break;
    }
    return S_OK;
}

HRESULT CALLBACK LinkProc(HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr,
                      SW_SHOWNORMAL);
    return S_OK;
}

}  // namespace

RuntimeChoice ChooseWebView2Runtime(HWND owner) {
    RuntimeChoice choice;

    // The installed runtime keeps itself up to date: always the first choice.
    // FLOATY_PRIVATE_RUNTIME=1 skips it, to test the bundled one on a machine
    // that has both.
    wchar_t force[8] = {0};
    const bool skipSystem =
        GetEnvironmentVariableW(L"FLOATY_PRIVATE_RUNTIME", force, 8) > 0 && force[0] == L'1';
    if (!skipSystem && IsRuntime(nullptr)) {
        choice.source = RuntimeSource::System;
        return choice;
    }

    const std::wstring root = RuntimeRoot();
    if (root.empty()) return choice;
    choice.folder = RuntimeInside(root);
    if (!choice.folder.empty()) {
        choice.source = RuntimeSource::Private;
        return choice;
    }

    UnpackJob job;
    job.exe = SelfPath();
    job.root = root;
    if (!FindPayload(job.exe, job.offset, job.size)) return choice;  // Missing

    std::thread worker([&job] {
        Unpack(job);
        job.done = true;
    });

    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof tc;
    tc.hwndParent = owner;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_RTL_LAYOUT |
                 TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = kTitle;
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = L"מכין את Floaty בפעם הראשונה";
    tc.pszContent =
        L"רכיב התצוגה של מיקרוסופט ארוז בתוך Floaty, ועכשיו הוא נפרס במחשב. "
        L"זה קורה פעם אחת בלבד ולוקח עד דקה.";
    tc.pfCallback = ProgressProc;
    tc.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);

    if (!job.done) {
        // Closed before the end: stop the unpack and wait for the worker to tidy up.
        job.cancel = true;
        std::lock_guard<std::mutex> g(job.lock);
        if (job.process) TerminateProcess(job.process, 1);
    }
    worker.join();

    if (job.cancel && job.result.empty()) {
        choice.source = RuntimeSource::Failed;  // the user's choice: nothing to report
        return choice;
    }
    if (job.result.empty()) {
        choice.source = RuntimeSource::Failed;
        choice.error = job.error;
        return choice;
    }
    choice.source = RuntimeSource::Private;
    choice.folder = job.result;
    return choice;
}

void ReportMissingRuntime(HWND owner) {
    const TASKDIALOG_BUTTON buttons[] = {
        {1001, L"להוריד את הרכיב מאתר מיקרוסופט\nצריך אינטרנט. אחרי ההתקנה פותחים את Floaty שוב"},
    };
    std::wstring content =
        L"חלון הניהול של Floaty מוצג בעזרת רכיב WebView2 של מיקרוסופט, "
        L"והוא לא מותקן במחשב הזה.\n\n"
        L"במחשב בלי אינטרנט: הורידו ממחשב אחר את <a href=\"";
    content += kReleases;
    content += L"\">Floaty-offline.exe</a>. הרכיב ארוז בתוכו.";

    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof tc;
    tc.hwndParent = owner;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ENABLE_HYPERLINKS | TDF_RTL_LAYOUT |
                 TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tc.pszWindowTitle = kTitle;
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = L"חסר רכיב של מיקרוסופט";
    tc.pszContent = content.c_str();
    tc.pButtons = buttons;
    tc.cButtons = 1;
    tc.pfCallback = LinkProc;
    int pressed = 0;
    TaskDialogIndirect(&tc, &pressed, nullptr, nullptr);
    if (pressed == 1001)
        ShellExecuteW(nullptr, L"open", kBootstrapper, nullptr, nullptr, SW_SHOWNORMAL);
}

void ReportRuntimeError(HWND owner, const std::wstring& what) {
    std::wstring content = what;
    content += L"\n\nאפשר לנסות לפתוח את Floaty שוב. אם זה חוזר, ";
    content += L"<a href=\"https://github.com/tsoolgee/overlay-manager-win/issues\">ספרו לנו</a>.";
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof tc;
    tc.hwndParent = owner;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_RTL_LAYOUT | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tc.pszWindowTitle = kTitle;
    tc.pszMainIcon = TD_ERROR_ICON;
    tc.pszMainInstruction = L"חלון הניהול לא נפתח";
    tc.pszContent = content.c_str();
    tc.pfCallback = LinkProc;
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
}
