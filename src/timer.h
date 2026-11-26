// timer.h — the kiosk timer: a built-in layer that is drawn rather than loaded
// from a picture. A compact bar with the time left; hovering opens the full
// panel; a right click opens its menu. Its buttons are decoration only.
#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "model.h"

namespace Gdiplus {
class Graphics;
class Bitmap;
}  // namespace Gdiplus

// The image value of a timer layer.
extern const wchar_t* const kTimerImage;
inline bool IsTimerImage(const std::wstring& image) { return image == kTimerImage; }

// What a new timer counts down from: 2:43:28, as the original program did.
constexpr int kTimerDefaultStart = 9808;

// Its usual place: this many percent from the top-left corner.
constexpr double kTimerDefaultOffset = 1.0;

namespace timerface {

enum class Part { None, Exit, Printer, Account, Dots };

// Window size in pixels at a DPI scale (1.0 = 96 DPI).
SIZE Size(bool expanded, double scale);

// Which decoration is under a point in window coordinates.
Part HitTest(bool expanded, double scale, int x, int y);

void Draw(Gdiplus::Graphics& g, bool expanded, double scale, Part hover, double remaining,
          double used);

// The compact bar at 100%, for the previews in the manager window.
std::unique_ptr<Gdiplus::Bitmap> Preview();

// "02:41:07"
std::wstring Format(double seconds);
// "2:30", "2:30:00", "1.15" (h, h:mm or h:mm:ss) to seconds; false if not a time.
bool Parse(const std::wstring& text, int& seconds);

// A small modal prompt for a time; false if cancelled.
bool AskTime(HWND owner, const std::wstring& title, int initial, int& out);

}  // namespace timerface
