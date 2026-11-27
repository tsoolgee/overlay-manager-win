// service.h — the Windows service behind the all-users mode (see allusers.h).
//
// It runs as SYSTEM from the copy in Program Files and does three things:
//   * starts the program, as the user, in every session where someone is
//     logged on and it is not running (a logon, or it was killed);
//   * leaves alone a session whose manager closed it on purpose;
//   * installs updates, since only it may replace the file in Program Files.
#pragma once

#include <string>

namespace svc {

extern const wchar_t* const kServiceName;

// "/service": hands the process to the service control manager. Returns when
// the service stops; non-zero if this was not started as a service.
int Main();

// Creates the service (or points the existing one at exe) and starts it.
bool Install(const std::wstring& exe);
// Stops and deletes it; true if it is gone (or was never there).
bool Remove();

}  // namespace svc
