// util.h — small string and shell helpers shared across the app.
#pragma once

#include <windows.h>

#include <string>

std::string  ToUtf8(const std::wstring& s);
std::wstring ToWide(const std::string& s);

// Full path of the running executable.
std::wstring ExePath();

// HKCU\...\CurrentVersion\Run entry, so autostart needs no elevation.
bool  IsAutoStartEnabled();
void  SetAutoStartEnabled(bool enabled);
// The executable the autostart entry points to; empty when there is none.
std::wstring AutoStartTarget();
