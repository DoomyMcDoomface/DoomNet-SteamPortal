// Spec: Installer.
// Shown once, later. This file is the decision only: no window, no shortcut
// change, no write into Steam. Color is asked only when both pieces are
// selected, and it is written only when settings.json does not exist.
// Player-only is Steam Blue ("steam"). Match Steam-Skin is "red".

#include "installer.h"

#include "anchors.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace doomnet {
namespace {

void sync_color_controls(HWND window) {
    const bool player = SendMessageW(GetDlgItem(window, kInstallIdPlayer), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool skin = SendMessageW(GetDlgItem(window, kInstallIdSkin), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool both = player && skin;
    EnableWindow(GetDlgItem(window, kInstallIdSteamBlue), both);
    EnableWindow(GetDlgItem(window, kInstallIdMatch), both);
    if (!both) {
        CheckRadioButton(window, kInstallIdSteamBlue, kInstallIdMatch, kInstallIdSteamBlue);
    }
}

LRESULT CALLBACK install_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    InstallChoices* choices = reinterpret_cast<InstallChoices*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
        choices = reinterpret_cast<InstallChoices*>(created->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(choices));
        const HINSTANCE instance = created->hInstance;
        CreateWindowExW(0, L"BUTTON", L"Player", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 16, 380, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallIdPlayer)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Skin", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 44, 380, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallIdSkin)), instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"Player color", WS_CHILD | WS_VISIBLE, 16, 80, 380, 20, window, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Steam Blue (default)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 16, 104, 380, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallIdSteamBlue)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Match Steam-Skin", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 16, 132, 380, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallIdMatch)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 16, 176, 120, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallIdInstall)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 148, 176, 120, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallIdCancel)), instance, nullptr);
        CheckRadioButton(window, kInstallIdSteamBlue, kInstallIdMatch, kInstallIdSteamBlue);
        sync_color_controls(window);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == kInstallIdPlayer || id == kInstallIdSkin) {
            sync_color_controls(window);
            return 0;
        }
        if (id == kInstallIdCancel) {
            if (choices) {
                choices->accepted = false;
            }
            log_event(L"info", L"installer", L"Setup was cancelled. No files were written.");
            DestroyWindow(window);
            return 0;
        }
        if (id == kInstallIdInstall && choices) {
            choices->player = SendMessageW(GetDlgItem(window, kInstallIdPlayer), BM_GETCHECK, 0, 0) == BST_CHECKED;
            choices->skin = SendMessageW(GetDlgItem(window, kInstallIdSkin), BM_GETCHECK, 0, 0) == BST_CHECKED;
            choices->match_skin = choices->player && choices->skin && SendMessageW(GetDlgItem(window, kInstallIdMatch), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!choices->player && !choices->skin) {
                log_event(L"error", L"installer", L"Install was clicked with neither the player nor the skin selected. Nothing was written.");
                return 0;
            }
            choices->accepted = true;
            const std::wstring detail = std::wstring(L"Setup accepted. Player is ") + (choices->player ? L"on" : L"off") + L", skin is " + (choices->skin ? L"on" : L"off") + (choices->match_skin ? L", player color matches Steam-Skin." : L", player color is Steam Blue when a new settings file is created.");
            log_event(L"info", L"installer", detail.c_str());
            DestroyWindow(window);
            return 0;
        }
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace

bool color_question_needed(bool player, bool skin) {
    return player && skin;
}

const wchar_t* theme_for_new_settings(bool player, bool skin, bool match_skin, bool settings_exist) {
    if (settings_exist || !player) {
        return nullptr;
    }
    if (!skin) {
        return L"steam";
    }
    return match_skin ? L"red" : L"steam";
}

HWND create_install_window(InstallChoices* choices) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = install_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"DoomNetInstall";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, L"DoomNetInstall", L"DoomNet Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, -32000, -32000, 440, 260, nullptr, nullptr, instance, choices);
    if (!window) {
        log_event(L"error", L"installer", (L"Could not create the setup window. " + windows_error_text(GetLastError())).c_str());
        return nullptr;
    }
    ShowWindow(window, SW_HIDE);
    log_event(L"info", L"installer", L"Setup window created and kept hidden. Steam was not started.");
    return window;
}

void installer_anchor() {}

}  // namespace doomnet

