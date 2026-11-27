// model.h — the overlay layer record and the app-wide settings.
#pragma once

#include <string>
#include <vector>

// Where a layer is pinned on its monitor. Offsets are then applied as a
// percentage of the monitor size, which keeps a layout usable across
// differently sized screens.
enum AnchorH { ANCHOR_LEFT = 0, ANCHOR_HCENTER = 1, ANCHOR_RIGHT = 2 };
enum AnchorV { ANCHOR_TOP = 0, ANCHOR_VCENTER = 1, ANCHOR_BOTTOM = 2 };

struct Layer {
    std::wstring name = L"שכבה חדשה";
    std::wstring image;
    bool enabled = true;

    int width = 200;
    int height = 200;
    bool lockAspect = true;

    int monitor = 0;             // index into Monitors
    int anchorH = ANCHOR_RIGHT;
    int anchorV = ANCHOR_BOTTOM;
    double offsetX = 2.0;        // percent of monitor width
    double offsetY = 2.0;        // percent of monitor height

    int opacity = 255;           // 0..255
    bool useTint = false;
    int tintR = 255, tintG = 255, tintB = 255;

    // Added in the rewrite; absent from an older config.ini, which is fine
    // because the loader falls back to these defaults.
    double rotation = 0.0;       // degrees, clockwise
    bool clickThrough = true;    // pass mouse events to whatever is underneath
    bool grayscale = false;

    // Turns the flat background of a logo saved without transparency (a white
    // or coloured square) see-through. The colour is taken from the image's
    // corners, and only the area connected to the edges is cleared, so the
    // same colour inside the logo survives.
    bool removeBg = false;
    int bgTolerance = 25;        // 1..100

    // How long this layer has been on screen, for the statistics in the
    // settings. Counted only while it is enabled, has a picture and
    // "hide all" is off.
    double usageSeconds = 0.0;

    // The kiosk timer (image "widget:timer", see timer.h): what "reset" goes
    // back to, the time left, the time used, and the switches in its menu.
    int timerStart = 9808;
    double timerRemaining = 9808;
    double timerUsed = 0.0;
    bool timerRunning = true;
    bool timerLocked = false;
};

struct Settings {
    bool autoStart = false;
    bool minimizeToTray = true;
    bool showTrayIcon = true;
    bool lightTheme = false;
    bool autoUpdate = true;      // check GitHub Releases and install newer builds
    // Global show/hide hotkey, stored as MOD_* flags plus a virtual key.
    unsigned hotkeyMods = 0;
    unsigned hotkeyVk = 0;
    // Global "show the next layer, hide the others" hotkey; vk 0 = off.
    unsigned cycleMods = 0;
    unsigned cycleVk = 0;
    std::string statsSince;      // "YYYY-MM-DD": when usage counting began
};

// Loads/saves %APPDATA%\OverlayManager\config.ini in the same layout the
// original build wrote, so an existing configuration keeps working.
bool LoadConfig(Settings& settings, std::vector<Layer>& layers);
bool SaveConfig(const Settings& settings, const std::vector<Layer>& layers);

std::wstring ConfigPath();
std::wstring ConfigDir();
// %APPDATA%\OverlayManager, whatever ConfigDir() currently points at.
std::wstring PerUserConfigDir();
// Points ConfigDir() elsewhere: the machine-wide folder when the program is
// installed for every user. Empty goes back to the default.
void SetConfigDir(const std::wstring& dir);
// OVERLAY_MANAGER_CONFIG_DIR is set: a test run with a folder of its own,
// which must not touch the real autostart entry either.
bool ConfigDirFromEnvironment();
// Another user's view of a machine-wide configuration: it may read it but not
// write it, so saving quietly does nothing instead of failing over and over.
void SetConfigReadOnly(bool readOnly);
