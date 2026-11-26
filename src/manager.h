// manager.h — the manager window: a frame around a WebView2 that renders ui/index.html.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

class ManagerWindow {
public:
    // Called with a message that arrived from the page, already parsed by the
    // caller's protocol handler.
    using MessageFn = std::function<void(const std::string& json)>;
    using CloseFn = std::function<void()>;

    static void RegisterWindowClass(HINSTANCE inst);

    ManagerWindow(HINSTANCE inst, MessageFn onMessage, CloseFn onClose);
    ~ManagerWindow();

    // Creates the window and starts the WebView2 environment. Returns false and
    // reports the reason if the runtime is missing.
    bool Create(bool lightTheme);

    void Show();
    void Hide();
    bool IsVisible() const;

    // Follows the page's theme with the title bar and the colour behind the page.
    void SetTheme(bool light);
    HWND Hwnd() const { return hwnd_; }

    // Queues a JSON message to the page; sends are buffered until the WebView
    // has finished loading, so start-up ordering cannot drop the first update.
    void Post(const std::string& json);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void ResizeWebView();
    void FlushQueue();
    void ApplyWebViewBackground();

    bool light_ = false;

    struct Impl;
    Impl* impl_ = nullptr;  // holds the WebView2 COM pointers

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    MessageFn onMessage_;
    CloseFn onClose_;
};

// True when a WebView2 runtime is installed. Checked before creating the window
// so the failure can be explained instead of appearing as a blank frame.
bool WebView2Available();
