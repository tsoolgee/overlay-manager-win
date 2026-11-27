// runtime.h — where the WebView2 runtime that draws the manager window comes from.
//
// Windows 11 and an up-to-date Windows 10 have the Evergreen runtime installed;
// Overlay Manager uses it. A Windows 10 that never went online may not, so
// OverlayManager-offline.exe carries Microsoft's Fixed Version runtime (a .cab) appended
// to the executable. On such a machine it is unpacked once into
// %LOCALAPPDATA%\OverlayManager\WebView2Runtime and used from there; a later, smaller
// OverlayManager.exe (an update) keeps finding it in the same place.
#pragma once

#include <windows.h>

#include <string>

enum class RuntimeSource { System, Private, Missing, Failed };

struct RuntimeChoice {
    RuntimeSource source = RuntimeSource::Missing;
    std::wstring folder;  // the private runtime's folder; empty for the system's
    std::wstring error;   // what went wrong, for RuntimeSource::Failed
};

// Finds a runtime, unpacking the bundled one first if that is the only way.
// Unpacking shows a progress dialog owned by `owner` and takes up to a minute.
RuntimeChoice ChooseWebView2Runtime(HWND owner);

// Explains a missing runtime: a download link for a connected machine, and
// OverlayManager-offline.exe for one without internet.
void ReportMissingRuntime(HWND owner);

// The runtime is there but could not be prepared or started.
void ReportRuntimeError(HWND owner, const std::wstring& what);
