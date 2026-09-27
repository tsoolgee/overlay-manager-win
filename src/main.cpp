#include <windows.h>
#include <objbase.h>

#include "app.h"

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
    EnsureDpiAwareness();

    // WebView2 and the shell dialogs both need COM on this thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const bool startMinimized = cmdLine && wcsstr(cmdLine, L"/minimized") != nullptr;
    const bool afterUpdate = cmdLine && wcsstr(cmdLine, L"/updated") != nullptr;

    int code = 0;
    {
        App app(inst);
        if (app.Start(startMinimized, afterUpdate)) code = app.Run();
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}
