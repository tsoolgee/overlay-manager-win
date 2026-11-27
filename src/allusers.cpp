#include "allusers.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wtsapi32.h>

#include <cwctype>
#include <vector>

#include "model.h"
#include "overlay.h"
#include "service.h"
#include "timer.h"
#include "util.h"

namespace allusers {
namespace {

const wchar_t* kVendor = L"TSOOL GEE";
const wchar_t* kProduct = L"Overlay Manager";
const wchar_t* kExeName = L"OverlayManager.exe";

std::wstring KnownFolder(int csidl) {
    wchar_t buf[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) return {};
    return buf;
}

std::wstring MarkerPath() { return MachineDir() + L"\\allusers.ini"; }

std::wstring SessionFlagPath(DWORD session) {
    return MachineDir() + L"\\session-" + std::to_wstring(session) + L".exited";
}

// The logon time of a session: what tells this logon from the next one in
// the same session number.
long long SessionLogonTime(DWORD session) {
    WTSINFOW* info = nullptr;
    DWORD bytes = 0;
    long long t = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfo,
                                    reinterpret_cast<LPWSTR*>(&info), &bytes) &&
        info) {
        t = info->LogonTime.QuadPart;
        WTSFreeMemory(info);
    }
    return t;
}

// Creates a folder and any missing parents.
bool CreateDirs(const std::wstring& dir) {
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

// "S-1-5-21-..." and nothing else: it goes into an SDDL string below.
bool LooksLikeSid(const std::wstring& sid) {
    if (sid.size() < 5 || sid.compare(0, 4, L"S-1-") != 0) return false;
    for (wchar_t c : sid)
        if (!(std::iswdigit(c) || c == L'-' || c == L'S')) return false;
    PSID p = nullptr;
    if (!ConvertStringSidToSidW(sid.c_str(), &p)) return false;
    LocalFree(p);
    return true;
}

// SYSTEM and administrators: full control. The managing user: modify.
// Everyone who can log on: read. Not inherited from ProgramData, whose own
// rules let any user create files there.
bool SetDataAcl(const std::wstring& dir, const std::wstring& ownerSid) {
    const std::wstring sddl = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;" +
                              ownerSid + L")(A;OICI;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                              nullptr))
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present;
    if (ok) {
        std::wstring path = dir;  // SetNamedSecurityInfo wants a writable string
        ok = SetNamedSecurityInfoW(&path[0], SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(sd);
    return ok;
}

std::wstring ShortcutPath() {
    const std::wstring programs = KnownFolder(CSIDL_COMMON_PROGRAMS);
    return programs.empty() ? std::wstring() : programs + L"\\" + kProduct + L".lnk";
}

// A Start menu entry for everyone: how the manager opens the program again
// after closing it.
void MakeShortcut() {
    const std::wstring lnk = ShortcutPath();
    if (lnk.empty()) return;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return;
    link->SetPath(MachineExe().c_str());
    link->SetWorkingDirectory(ProgramDir().c_str());
    link->SetDescription(L"Overlay Manager");
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

bool SameContent(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa) ||
        !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb))
        return false;
    if (fa.nFileSizeHigh != fb.nFileSizeHigh || fa.nFileSizeLow != fb.nFileSizeLow) return false;
    auto read = [](const std::wstring& p, std::vector<char>& out) {
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        if (size.QuadPart > 256LL * 1024 * 1024) {
            CloseHandle(f);
            return false;
        }
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        const bool ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) &&
                                        got == out.size());
        CloseHandle(f);
        return ok;
    };
    std::vector<char> da, db;
    return read(a, da) && read(b, db) && da == db;
}

}  // namespace

std::wstring ProgramDir() {
    return KnownFolder(CSIDL_PROGRAM_FILES) + L"\\" + kVendor + L"\\" + kProduct;
}
std::wstring MachineExe() { return ProgramDir() + L"\\" + kExeName; }
std::wstring MachineDir() {
    return KnownFolder(CSIDL_COMMON_APPDATA) + L"\\" + kVendor + L"\\" + kProduct;
}
std::wstring ImagesDir() { return MachineDir() + L"\\images"; }

std::wstring PersonalExe() {
    return KnownFolder(CSIDL_LOCAL_APPDATA) + L"\\Programs\\" + kProduct + L"\\" + kExeName;
}

bool Enabled() { return GetFileAttributesW(MarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsMachineCopy() { return _wcsicmp(ExePath().c_str(), MachineExe().c_str()) == 0; }

bool CanWriteMachineDir() {
    // Try it rather than read the rules: an administrator's own account may
    // still be refused while not elevated, and that is what counts here.
    const std::wstring probe =
        MachineDir() + L"\\.write-test-" + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::wstring out;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

DWORD CurrentSessionId() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

void MarkSessionExited() {
    const DWORD session = CurrentSessionId();
    const std::wstring text = std::to_wstring(SessionLogonTime(session));
    HANDLE f = CreateFileW(SessionFlagPath(session).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string bytes = ToUtf8(text);
    DWORD written = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(f);
}

void ClearSessionExited() { DeleteFileW(SessionFlagPath(CurrentSessionId()).c_str()); }

bool SessionExited(DWORD session) {
    HANDLE f = CreateFileW(SessionFlagPath(session).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[64] = {0};
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(f);
    // Only for the logon that closed it; a new logon gets the program again.
    return _atoi64(buf) == SessionLogonTime(session);
}

std::wstring ShareImage(const std::wstring& path) {
    if (path.empty() || IsStickerPath(path) || IsTimerImage(path)) return path;
    const std::wstring dir = ImagesDir();
    if (_wcsnicmp(path.c_str(), (dir + L"\\").c_str(), dir.size() + 1) == 0) return path;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    CreateDirs(dir);

    const std::wstring name = PathFindFileNameW(path.c_str());
    std::wstring stem = name, ext;
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int n = 1; n < 1000; ++n) {
        const std::wstring dst =
            dir + L"\\" + (n == 1 ? name : stem + L" (" + std::to_wstring(n) + L")" + ext);
        if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (SameContent(path, dst)) return dst;  // already shared
            continue;                                // another picture by that name
        }
        return CopyFileW(path.c_str(), dst.c_str(), TRUE) ? dst : path;
    }
    return path;
}

bool Launch(const std::wstring& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int RunElevatedAndWait(const std::wstring& args) {
    const std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? -1 : 99;
    DWORD code = 99;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    return (int)code;
}

int EnableElevated(const std::wstring& ownerSid, const std::wstring& sourceConfigDir) {
    if (!LooksLikeSid(ownerSid)) return 5;

    // 1. The program, where only administrators can change it.
    if (!CreateDirs(ProgramDir())) return 1;
    const std::wstring self = ExePath(), dst = MachineExe();
    if (_wcsicmp(self.c_str(), dst.c_str()) != 0 && !CopyFileW(self.c_str(), dst.c_str(), FALSE)) {
        // An earlier copy is running (the service, or a session): a running
        // executable can be renamed, though not overwritten.
        const std::wstring old = dst + L".old";
        DeleteFileW(old.c_str());
        if (!MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) ||
            !CopyFileW(self.c_str(), dst.c_str(), FALSE))
            return 1;
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    // 2. The shared settings folder and who may do what in it.
    if (!CreateDirs(MachineDir()) || !SetDataAcl(MachineDir(), ownerSid)) return 2;
    CreateDirs(ImagesDir());

    // 3. The manager's current layers become everyone's, with the pictures
    //    copied where every user can read them.
    Settings settings;
    std::vector<Layer> layers;
    if (!sourceConfigDir.empty()) {
        SetConfigDir(sourceConfigDir);
        if (LoadConfig(settings, layers)) {
            for (auto& l : layers) l.image = ShareImage(l.image);
            settings.autoStart = false;  // the service starts it now
            SetConfigDir(MachineDir());
            if (!SaveConfig(settings, layers)) return 3;
        }
    }
    SetConfigDir(MachineDir());
    if (!WritePrivateProfileStringW(L"AllUsers", L"Owner", ownerSid.c_str(), MarkerPath().c_str()))
        return 3;

    // 4. The Start menu entry and the service.
    MakeShortcut();
    if (!svc::Install(dst)) return 4;
    return 0;
}

int DisableElevated() {
    if (!svc::Remove()) return 4;
    DeleteFileW(MarkerPath().c_str());
    if (Enabled()) return 2;
    const std::wstring lnk = ShortcutPath();
    if (!lnk.empty()) DeleteFileW(lnk.c_str());

    // The copy in Program Files goes; one still running somewhere (it exits
    // within seconds of seeing the marker gone) goes at the next restart.
    for (const std::wstring& f : {MachineExe(), MachineExe() + L".old"})
        if (!DeleteFileW(f.c_str()) && GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExW(f.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!RemoveDirectoryW(ProgramDir().c_str()))
        MoveFileExW(ProgramDir().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    // ProgramData stays: the pictures in it are where the manager's layers
    // still point.
    return 0;
}

std::string ErrorText(int code) {
    switch (code) {
        case -1: return "הפעולה בוטלה בחלון ההרשאות של Windows";
        case 1:  return "לא ניתן היה להעתיק את התוכנה לתיקיית Program Files";
        case 2:  return "לא ניתן היה להכין את תיקיית ההגדרות המשותפת";
        case 3:  return "לא ניתן היה להעביר את השכבות להגדרות המשותפות";
        case 4:  return "לא ניתן היה להתקין או להסיר את השירות של Windows";
        case 5:  return "לא ניתן היה לזהות את המשתמש הנוכחי";
        default: return "הפעולה נכשלה (קוד " + std::to_string(code) + ")";
    }
}

}  // namespace allusers
