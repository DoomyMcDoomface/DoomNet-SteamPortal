#include "folder_pick.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace doomnet {
namespace {

enum class PickState { Idle, Running, Path, Cancel, Failed };

std::mutex g_mu;
PickState g_state = PickState::Idle;
std::wstring g_path;
std::wstring g_error;
std::thread g_thread;

std::wstring com_failure(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return windows_error_text(static_cast<unsigned long>(HRESULT_CODE(hr)));
    }
    wchar_t buffer[96] = {};
    swprintf_s(buffer, L"HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

void finish(PickState state, const std::wstring& path, const std::wstring& error) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_state = state;
    g_path = path;
    g_error = error;
}

void run_dialog() {
    const HRESULT start = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = start == S_OK || start == S_FALSE;
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr) {
        finish(PickState::Failed, L"", L"The music folder chooser could not be opened. " + com_failure(hr));
    } else {
        DWORD options = 0;
        dialog->GetOptions(&options);
        hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (FAILED(hr)) {
            finish(PickState::Failed, L"", L"The music folder chooser could not be limited to folders. " + com_failure(hr));
        } else {
            dialog->SetTitle(L"Choose the folder that contains your music");
            hr = dialog->Show(GetForegroundWindow());
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                finish(PickState::Cancel, L"", L"");
            } else if (FAILED(hr)) {
                finish(PickState::Failed, L"", L"The music folder chooser failed. " + com_failure(hr));
            } else {
                IShellItem* item = nullptr;
                hr = dialog->GetResult(&item);
                PWSTR chosen = nullptr;
                if (FAILED(hr) || item == nullptr) {
                    finish(PickState::Failed, L"", L"The music folder chooser did not return a folder. " + com_failure(hr));
                } else if (FAILED(hr = item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) || chosen == nullptr) {
                    finish(PickState::Failed, L"", L"The music folder chooser returned a folder with no path. " + com_failure(hr));
                } else {
                    finish(PickState::Path, chosen, L"");
                    CoTaskMemFree(chosen);
                }
                if (item != nullptr) {
                    item->Release();
                }
            }
        }
        dialog->Release();
    }
    if (uninit) {
        CoUninitialize();
    }
}

}  // namespace

bool begin_folder_pick() {
    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state == PickState::Running) {
            log_event(L"warn", L"lua", L"The music folder chooser is already open, so a second one was not started.");
            return false;
        }
        if (g_thread.joinable()) {
            previous.swap(g_thread);
        }
        g_path.clear();
        g_error.clear();
        g_state = PickState::Running;
        g_thread = std::thread(run_dialog);
    }
    if (previous.joinable()) {
        previous.join();
    }
    log_event(L"info", L"lua", L"Opened the music folder chooser.");
    return true;
}

std::string folder_pick_take(std::wstring& path, std::wstring& error) {
    path.clear();
    error.clear();
    std::thread done;
    PickState state = PickState::Idle;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state == PickState::Running) {
            return "wait";
        }
        if (g_state == PickState::Idle) {
            return "idle";
        }
        state = g_state;
        path = g_path;
        error = g_error;
        g_state = PickState::Idle;
        g_path.clear();
        g_error.clear();
        done.swap(g_thread);
    }
    if (done.joinable()) {
        done.join();
    }
    if (state == PickState::Path) {
        log_event(L"info", L"lua", (L"The music folder chooser returned '" + path + L"'.").c_str());
        return "path";
    }
    if (state == PickState::Cancel) {
        log_event(L"info", L"lua", L"The music folder chooser was closed without a folder.");
        return "cancel";
    }
    log_event(L"error", L"lua", error.c_str());
    return "failed";
}

}  // namespace doomnet
