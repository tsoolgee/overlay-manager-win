#include "service.h"

#include <windows.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <vector>

#include "allusers.h"
#include "model.h"
#include "updater.h"
#include "util.h"

namespace svc {

const wchar_t* const kServiceName = L"OverlayManagerAllUsers";

namespace {

const wchar_t* kDisplayName = L"Overlay Manager - כל המשתמשים";
const wchar_t* kDescription =
    L"מפעיל את Overlay Manager אצל כל משתמש שנכנס למחשב, כך שהשכבות מוצגות לכולם, "
    L"ומתקין עדכונים של התוכנה.";

const DWORD kTickMs = 5000;                        // how often sessions are looked at
const ULONGLONG kFirstUpdateMs = 2 * 60 * 1000;    // first update check after start
const ULONGLONG kUpdateEveryMs = 6ULL * 60 * 60 * 1000;

const wchar_t* PathFindName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
}

SERVICE_STATUS_HANDLE g_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop = nullptr;   // manual reset: the service is stopping
HANDLE g_wake = nullptr;   // auto reset: someone logged on, look now

void Report(DWORD state) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE
            : 0;
    g_status.dwWin32ExitCode = NO_ERROR;
    g_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 10000;
    if (g_handle) SetServiceStatus(g_handle, &g_status);
}

DWORD WINAPI Handler(DWORD control, DWORD type, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            Report(SERVICE_STOP_PENDING);
            SetEvent(g_stop);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            if (type == WTS_SESSION_LOGON || type == WTS_CONSOLE_CONNECT ||
                type == WTS_REMOTE_CONNECT || type == WTS_SESSION_UNLOCK)
                SetEvent(g_wake);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Is the installed copy running in this session? By the full path, so a
// personal copy with the same file name does not count.
bool RunningInSession(DWORD session, const std::wstring& exe) {
    const wchar_t* name = PathFindName(exe);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;  // cannot tell: do not start a second one
    bool found = false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) != 0) continue;
        DWORD s = 0;
        if (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != session) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH * 2] = {0};
        DWORD len = (DWORD)std::size(path);
        if (QueryFullProcessImageNameW(p, 0, path, &len) && _wcsicmp(path, exe.c_str()) == 0)
            found = true;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}

// Starts the program in a session, as the user logged on there, on their
// desktop, with their environment.
bool LaunchInSession(DWORD session, const std::wstring& exe) {
    HANDLE user = nullptr;
    if (!WTSQueryUserToken(session, &user)) return false;
    HANDLE primary = nullptr;
    const bool dup = DuplicateTokenEx(user, MAXIMUM_ALLOWED, nullptr, SecurityIdentification,
                                      TokenPrimary, &primary) != FALSE;
    CloseHandle(user);
    if (!dup) return false;

    void* env = nullptr;
    CreateEnvironmentBlock(&env, primary, FALSE);
    std::wstring cmd = L"\"" + exe + L"\" /minimized";
    std::wstring desktop = L"winsta0\\default";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.lpDesktop = &desktop[0];
    PROCESS_INFORMATION pi{};
    const std::wstring dir = allusers::ProgramDir();
    const bool ok = CreateProcessAsUserW(primary, nullptr, &cmd[0], nullptr, nullptr, FALSE,
                                         CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS, env,
                                         dir.c_str(), &si, &pi) != FALSE;
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(primary);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok;
}

// Per session: when the program was last started there and how often it has
// died right after, so a program that crashes on start is not restarted in a
// tight loop.
struct SessionState {
    ULONGLONG lastLaunch = 0;
    int quickFailures = 0;
};
std::map<DWORD, SessionState> g_sessions;

void LookAtSessions() {
    if (!allusers::Enabled()) return;
    const std::wstring exe = allusers::MachineExe();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    WTS_SESSION_INFOW* list = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &list, &count)) return;
    std::map<DWORD, SessionState> seen;
    const ULONGLONG now = GetTickCount64();
    for (DWORD k = 0; k < count; ++k) {
        const DWORD id = list[k].SessionId;
        // Session 0 is services; a disconnected session keeps what it runs.
        if (id == 0 || list[k].State != WTSActive) continue;
        SessionState st = g_sessions.count(id) ? g_sessions[id] : SessionState{};
        if (!RunningInSession(id, exe) && !allusers::SessionExited(id)) {
            // Died within a minute of the last start: back off, doubling.
            if (st.lastLaunch && now - st.lastLaunch < 60000) {
                const ULONGLONG wait = std::min<ULONGLONG>(5000ULL << std::min(st.quickFailures, 7), 600000);
                if (now - st.lastLaunch < wait) {
                    seen[id] = st;
                    continue;
                }
                ++st.quickFailures;
            } else {
                st.quickFailures = 0;
            }
            if (LaunchInSession(id, exe)) st.lastLaunch = now;
        }
        seen[id] = st;
    }
    WTSFreeMemory(list);
    g_sessions.swap(seen);  // forget sessions that ended
}

// Only the service may replace the file in Program Files. After a successful
// swap it exits without reporting "stopped", which the service control
// manager treats as a failure and answers by restarting it (see Install) -
// now from the new file. The copies in the sessions notice the file changed
// and restart themselves.
void MaybeUpdate() {
    Settings settings;
    std::vector<Layer> layers;
    if (LoadConfig(settings, layers) && !settings.autoUpdate) return;
    const UpdateInfo info = CheckForUpdate();
    if (!info.ok || !info.newer) return;
    if (DownloadAndInstall(info).empty()) ExitProcess(1);
}

void WINAPI ServiceMainProc(DWORD, LPWSTR*) {
    g_handle = RegisterServiceCtrlHandlerExW(kServiceName, &Handler, nullptr);
    if (!g_handle) return;
    Report(SERVICE_START_PENDING);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SetConfigDir(allusers::MachineDir());
    CleanupAfterUpdate();  // the file the last update moved aside
    Report(SERVICE_RUNNING);

    // A moment before the first look: when the service was just installed,
    // the copy that installed it is still handing its session over.
    const HANDLE waits[2] = {g_stop, g_wake};
    if (WaitForSingleObject(g_stop, 3000) != WAIT_OBJECT_0) {
        ULONGLONG nextUpdate = GetTickCount64() + kFirstUpdateMs;
        for (;;) {
            LookAtSessions();
            if (GetTickCount64() >= nextUpdate) {
                nextUpdate = GetTickCount64() + kUpdateEveryMs;
                MaybeUpdate();
            }
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, kTickMs);
            if (w == WAIT_OBJECT_0) break;
            // A logon: the user's desktop needs a moment before it takes a window.
            if (w == WAIT_OBJECT_0 + 1 && WaitForSingleObject(g_stop, 1500) == WAIT_OBJECT_0) break;
        }
    }
    Report(SERVICE_STOPPED);
}

bool WaitForState(SC_HANDLE service, DWORD state, DWORD ms) {
    SERVICE_STATUS st{};
    for (DWORD waited = 0;; waited += 250) {
        if (!QueryServiceStatus(service, &st)) return false;
        if (st.dwCurrentState == state) return true;
        if (waited >= ms) return false;
        Sleep(250);
    }
}

}  // namespace

int Main() {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), &ServiceMainProc},
                                    {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

bool Install(const std::wstring& exe) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;
    const std::wstring bin = L"\"" + exe + L"\" /service";
    SC_HANDLE s = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
                                 SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    bool existed = false;
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {
        existed = true;
        s = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (s)
            ChangeServiceConfigW(s, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kDisplayName);
    }
    if (!s) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_DESCRIPTIONW desc{const_cast<LPWSTR>(kDescription)};
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &desc);
    // Restart after a crash, and after the deliberate exit that follows an update.
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000},
                            {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 24 * 60 * 60;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    // A running service keeps the old file in memory: restart it on the new one.
    if (existed) {
        SERVICE_STATUS st{};
        if (ControlService(s, SERVICE_CONTROL_STOP, &st)) WaitForState(s, SERVICE_STOPPED, 15000);
    }
    const bool started = StartServiceW(s, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    const bool running = started && WaitForState(s, SERVICE_RUNNING, 15000);
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return running;
}

bool Remove() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!s) {
        const bool gone = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
        CloseServiceHandle(scm);
        return gone;
    }
    SERVICE_STATUS st{};
    if (ControlService(s, SERVICE_CONTROL_STOP, &st)) WaitForState(s, SERVICE_STOPPED, 15000);
    const bool ok = DeleteService(s) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

}  // namespace svc
