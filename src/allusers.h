// allusers.h — the program installed for every user of the computer.
//
// A service cannot draw on anyone's screen (it lives in session 0), so the
// service only makes sure the program runs in every user's session, as that
// user, with the usual tray icon. What it shows comes from one machine-wide
// configuration:
//
//   %ProgramFiles%\TSOOL GEE\Overlay Manager\OverlayManager.exe
//       the program; the service runs it and starts it in each session.
//       Only administrators can change it, since the service runs it as SYSTEM.
//   %ProgramData%\TSOOL GEE\Overlay Manager\
//       config.ini, images\ (copies of the layers' pictures, readable by all),
//       allusers.ini (the marker: on, and who manages it).
//       The user who turned it on may write here, everyone else only reads.
//
// The user who manages it edits the layers as before, without elevation.
// Every other user sees them and cannot change, hide or close them.
#pragma once

#include <windows.h>

#include <string>

namespace allusers {

std::wstring ProgramDir();   // %ProgramFiles%\TSOOL GEE\Overlay Manager
std::wstring MachineExe();   // ...\OverlayManager.exe in ProgramDir()
std::wstring MachineDir();   // %ProgramData%\TSOOL GEE\Overlay Manager
std::wstring ImagesDir();    // MachineDir()\images

// Installed for every user: the marker is there.
bool Enabled();
// This process is the installed copy, not a personal one somewhere else.
bool IsMachineCopy();
// This user may change the machine-wide layers.
bool CanWriteMachineDir();

std::wstring CurrentUserSid();
DWORD CurrentSessionId();

// The manager closed the program on purpose: the service leaves this session
// alone until its next logon, instead of starting the program right back.
void MarkSessionExited();
void ClearSessionExited();
bool SessionExited(DWORD session);

// A picture the other users can read: copied into ImagesDir() (once; the same
// file again maps to the same copy). Stickers, the timer and paths already in
// there are returned as they are, and so is the original if the copy fails.
std::wstring ShareImage(const std::wstring& path);

// Runs this executable elevated with the given arguments and waits for it.
// Returns its exit code, or -1 when the user declined the UAC prompt.
int RunElevatedAndWait(const std::wstring& args);

// The elevated halves, run as "/allusers-enable <sid> <config dir>" and
// "/allusers-disable". 0 = done; anything else is a code for ErrorText().
int EnableElevated(const std::wstring& ownerSid, const std::wstring& sourceConfigDir);
int DisableElevated();
std::string ErrorText(int code);  // Hebrew, UTF-8, for the page

// Where a manager's own copy goes when the mode is turned off: the copy in
// Program Files belongs to the administrators and cannot update itself.
std::wstring PersonalExe();   // %LOCALAPPDATA%\Programs\Overlay Manager\OverlayManager.exe

// Starts an executable in this session with a command line; false on failure.
bool Launch(const std::wstring& exe, const std::wstring& args);

}  // namespace allusers
