// updater.h — checks GitHub Releases for a newer build and swaps it in.
//
// The program is a single EXE, so updating means replacing that one file.
// Windows lets a running executable be renamed (not deleted), which is the
// whole trick: the running copy moves aside to "<exe>.old", the new one takes
// its name, and the next launch — or an immediate restart — runs it.
#pragma once

#include <string>

// "3.2.0", from CMakeLists.txt's project(VERSION) via the generated version.h.
const char* AppVersion();

struct UpdateInfo {
    bool ok = false;          // the check itself succeeded
    bool newer = false;       // the latest release is newer than this build
    std::string version;      // "3.2.0", without the leading "v"
    std::string downloadUrl;  // the OverlayManager.exe asset
    std::string sha256;       // upper-case hex; empty if the release has none
    std::string error;        // Hebrew, for the page, when !ok
};

// Blocking; run it off the UI thread.
UpdateInfo CheckForUpdate();

// Downloads info.downloadUrl, verifies its SHA-256, and puts it in place of
// the running executable. Blocking. Returns an empty string on success, or a
// Hebrew error for the page.
std::string DownloadAndInstall(const UpdateInfo& info);

// Starts the (already replaced) executable again, telling it to wait for this
// process to release the single-instance mutex rather than bail out.
bool RelaunchUpdated(bool minimized);

// Removes "<exe>.old" left behind by a previous update. The old process may
// still be exiting, so this retries on a background thread.
void CleanupAfterUpdate();
