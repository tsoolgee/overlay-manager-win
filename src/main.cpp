#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "allusers.h"
#include "app.h"
#include "service.h"

// The overlays are drawn from real screen rectangles, so the process has to see
// true pixels. The manifest asks for per-monitor v2; this call is the fallback
// for a host that ignores it, and must run before any window exists.
static void EnsureDpiAwareness() {
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto set = reinterpret_cast<SetCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set && set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();  // Windows 8.1 and earlier
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    // The service and the elevated all-users steps are this same file, run
    // with an argument. They show no window, so they skip the UI setup.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring verb = argv && argc > 1 ? argv[1] : L"";
    if (verb == L"/service") {
        LocalFree(argv);
        return svc::Main();
    }
    if (verb == L"/allusers-enable" || verb == L"/allusers-disable") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for the Start menu shortcut
        const int code = verb == L"/allusers-disable"
                             ? allusers::DisableElevated()
                             : allusers::EnableElevated(argc > 2 ? argv[2] : L"", argc > 3 ? argv[3] : L"");
        LocalFree(argv);
        CoUninitialize();
        return code;
    }
    if (argv) LocalFree(argv);

    EnsureDpiAwareness();

    // WebView2 and the shell dialogs both need COM on this thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const bool startMinimized = cmdLine && wcsstr(cmdLine, L"/minimized") != nullptr;
    // Started by a copy that is about to exit (an update, or a hand-over to or
    // from the all-users install): wait for it to let go of the session.
    const bool waitForPrevious = cmdLine && (wcsstr(cmdLine, L"/updated") != nullptr ||
                                             wcsstr(cmdLine, L"/handover") != nullptr);

    int code = 0;
    {
        App app(inst);
        if (app.Start(startMinimized, waitForPrevious)) code = app.Run();
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}
