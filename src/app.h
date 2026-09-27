// app.h — ties the model, the overlays and the manager UI together.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "manager.h"
#include "model.h"
#include "overlay.h"

class App {
public:
    explicit App(HINSTANCE inst);
    ~App();

    bool Start(bool startMinimized, bool afterUpdate);
    int Run();

private:
    // --- messages from the page ---
    void OnUiMessage(const std::string& json);
    void SendInit();
    void SendLayers(int selected);
    void SendPreview(const std::wstring& path, int bgTolerance);
    void SendUpdateState();
    void Send(const std::string& json);

    // --- overlays ---
    void RebuildOverlays();
    void SyncOverlay(int index);
    void SetAllHidden(bool hidden);
    void SetPositionMode(int index, bool on);

    // --- shell integration ---
    bool CreateTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon();
    void ShowTrayMenu();
    void RegisterHotkey();

    // --- updates ---
    void StartUpdateCheck(bool manual);
    void OnUpdateFinished(struct UpdateResult* result);
    void RestartIntoUpdate();

    void ShowManager();
    void RequestQuit();

    // --- dialogs, run on the UI thread in response to a page request ---
    void BrowseImage(int index);
    void PickTint(int index);

    static LRESULT CALLBACK HiddenProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE inst_ = nullptr;
    HWND hidden_ = nullptr;   // owns the tray icon, hotkey and display messages
    HANDLE mutex_ = nullptr;

    Settings settings_;
    std::vector<Layer> layers_;
    std::vector<std::unique_ptr<OverlayWindow>> overlays_;
    int selected_ = -1;
    int positionIndex_ = -1;
    bool allHidden_ = false;
    bool trayAdded_ = false;
    bool quitting_ = false;

    bool updateBusy_ = false;
    std::string updateReady_;    // version installed and waiting for a restart
    bool justUpdated_ = false;   // started by the updater; say so once

    std::unique_ptr<ManagerWindow> manager_;
};
