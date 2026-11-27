// app.h — ties the model, the overlays and the manager UI together.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "json.h"
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
    js::Value SettingsJson() const;
    void SendSettings();
    void SendInit();
    void SendLayers(int selected);
    void SendPreview(const std::wstring& path, int bgTolerance);
    void SendUpdateState();
    void Send(const std::string& json);

    // --- overlays ---
    void RebuildOverlays();
    void RestackOverlays();
    // Always on top: another program's window that went over a layer (a
    // topmost window, the taskbar...) is noticed and the layers go back above it.
    bool OverlaysCovered() const;
    void StartTopmostGuard();
    void StopTopmostGuard();
    void RaiseSoon();
    static void CALLBACK OnShellEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD thread, DWORD time);
    void SyncOverlay(int index);
    void OnTimerChanged(int index, const Layer& timer, bool save);
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
    void SetHotkey(bool cycle, unsigned mods, unsigned vk);
    void ShowNextLayer();

    // --- statistics and resets ---
    void AccountUsage();
    void SendStats();
    void ResetSettings();
    void ResetAll();
    void UndoReset();
    void Toast(const std::string& text, bool warn, const char* action = nullptr,
               const char* actionLabel = nullptr);

    // --- updates ---
    void StartUpdateCheck(bool manual, bool install);
    void OnUpdateFinished(struct UpdateResult* result);
    void RestartIntoUpdate();

    // --- installed for every user (allusers.h) ---
    void SetAllUsers(bool on);
    void OnAllUsersDone(bool on, int code);
    void MachineTick();
    void ReloadSharedConfig();
    void LeaveMachineMode();

    void ShowManager();
    void RequestQuit();
    // Closed from the tray or the window, rather than by an update or a
    // hand-over: in the all-users mode the service then leaves this session be.
    void ExitByUser();

    // --- dialogs, run on the UI thread in response to a page request ---
    void BrowseImage(int index);
    void SetLayerImage(int index, const std::wstring& image);
    void PickTint(int index);

    static LRESULT CALLBACK HiddenProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE inst_ = nullptr;
    HWND hidden_ = nullptr;   // owns the tray icon, hotkey and display messages
    HANDLE mutex_ = nullptr;
    HWINEVENTHOOK shellHooks_[3] = {};

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
    bool cycleOk_ = true;           // the same, for the next-layer key
    unsigned long long lastUsageTick_ = 0;  // GetTickCount64 at the last usage count
    int usageTicks_ = 0;                    // counts since the last save
    bool quitting_ = false;

    bool machine_ = false;        // the all-users install, reading the shared settings
    bool canManage_ = true;       // false: another user's read-only view of them
    bool allUsersBusy_ = false;   // waiting on the elevated step
    FILETIME exeStamp_{};         // the installed file, to notice an update
    FILETIME cfgStamp_{};         // the shared config.ini, to notice the manager's edits

    bool updateBusy_ = false;
    std::string updateReady_;      // version installed and waiting for a restart
    std::string updateAvailable_;  // newer version found but not installed
    std::string updateNotes_;      // its release notes, plain text
    std::string updateError_;      // why the last install failed, for the page
    UpdateRecord updated_;         // this start is the result of an update
    std::string updateFailedTo_;   // an installed update that did not take
    struct {
        long long atMs = 0;        // when the last check finished (Unix ms); 0 = none yet
        bool ok = false;
        std::string latest, published, error;
    } lastCheck_;

    std::unique_ptr<ManagerWindow> manager_;
};
