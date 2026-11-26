// updater.h — checks GitHub Releases for a newer build and swaps it in.
//
// The program is a single EXE, so updating means replacing that one file.
// Windows lets a running executable be renamed (not deleted), which is the
// whole trick: the running copy moves aside to "<exe>.old", the new one takes
// its name, and the next launch — or an immediate restart — runs it.
//
// For tests only, OVERLAY_UPDATE_API points the check at another "latest
// release" URL; plain http is then allowed for 127.0.0.1 / localhost, so a
// local server can stand in for GitHub and serve broken downloads on purpose.
#pragma once

#include <string>

// "3.2.0", from CMakeLists.txt's project(VERSION) via the generated version.h.
const char* AppVersion();

// The page where every release can be downloaded by hand.
std::string ReleasesPageUrl();

struct UpdateInfo {
    bool ok = false;          // the check itself succeeded
    bool newer = false;       // the latest release is newer than this build
    std::string version;      // "3.2.0", without the leading "v"
    std::string downloadUrl;  // the program's .exe asset
    std::string sha256;       // upper-case hex; empty if the release has none
    std::string notes;        // the release notes, as plain text, for "What's new"
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

// A note the installing version leaves for the next start: which version it
// put in place and what that release says. The next start reads it once:
// running that version means the update worked and "What's new" can be shown;
// running anything else means the swap did not take, which is then said out
// loud instead of the program quietly staying old.
struct UpdateRecord {
    bool present = false;
    std::string from, to, notes;
    std::string exe;  // the file that was replaced (UTF-8)
};
void WriteUpdateRecord(const UpdateInfo& info);
UpdateRecord TakeUpdateRecord();
