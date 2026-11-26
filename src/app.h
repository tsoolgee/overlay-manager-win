// app.h — ties the model, the overlays and the manager UI together.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "manager.h"
#include "model.h"
#include "overlay.h"
#include "updater.h"

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
    void RestackOverlays();
    void SyncOverlay(int index);
    void SetAllHidden(bool hidden);
    void SetPositionMode(int index, bool on);
    void CancelPositionMode();
    void RemapPositionIndex(int index);

    // --- shell integration ---
    bool CreateTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon();
    void ShowTrayMenu();
    void RegisterHotkey();

    // --- updates ---
    void StartUpdateCheck(bool manual, bool install);
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
    Layer positionBackup_;          // the positioned layer before it was moved
    bool positionWasHidden_ = false;
    bool allHidden_ = false;
    bool trayAdded_ = false;
    bool hotkeyOk_ = true;          // false while another program owns the combination
    bool quitting_ = false;

    bool updateBusy_ = false;
    std::string updateReady_;      // version installed and waiting for a restart
    std::string updateAvailable_;  // newer version found but not installed
    std::string updateNotes_;      // its release notes, plain text
    std::string updateError_;      // why the last install failed, for the page
    UpdateRecord updated_;         // this start is the result of an update
    std::string updateFailedTo_;   // an installed update that did not take

    std::unique_ptr<ManagerWindow> manager_;
};
