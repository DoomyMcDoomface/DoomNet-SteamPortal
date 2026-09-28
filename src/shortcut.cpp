#include "shortcut.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shlobj.h>

#include <string>

namespace doomnet {
namespace {

struct ComScope {
    bool uninit = false;
    ~ComScope() {
        if (uninit) {
            CoUninitialize();
        }
    }
};

bool start_com(ComScope& scope, std::wstring& error) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        error = L"COM is already in a different threading mode, so the shortcut could not be edited.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    if (FAILED(hr) && hr != S_FALSE) {
        error = L"CoInitializeEx failed while editing a shortcut. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    scope.uninit = hr == S_OK;
    return true;
}

bool file_name_is_steam(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (wchar_t& ch : name) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return name == L"steam.exe";
}

bool save_link(IShellLinkW* link, const std::wstring& lnk_path, std::wstring& error) {
    IPersistFile* file = nullptr;
    HRESULT hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (FAILED(hr) || !file) {
        error = L"The shortcut object at '" + lnk_path + L"' could not be saved.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    hr = file->Save(lnk_path.c_str(), TRUE);
    file->Release();
    if (FAILED(hr)) {
        error = L"Could not save shortcut '" + lnk_path + L"'. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    return true;
}

}  // namespace

bool create_shortcut(const std::wstring& lnk_path, const std::wstring& target, const std::wstring& icon_path, std::wstring& error) {
    error.clear();
    if (lnk_path.empty() || target.empty() || lnk_path.find(L"..") != std::wstring::npos) {
        error = L"Refusing to create shortcut '" + lnk_path + L"' because a path is empty or contains '..'.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    ComScope com;
    if (!start_com(com, error)) {
        return false;
    }
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        error = L"Could not create a shortcut object for '" + lnk_path + L"'. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    link->SetPath(target.c_str());
    if (!icon_path.empty()) {
        link->SetIconLocation(icon_path.c_str(), 0);
    }
    const bool saved = save_link(link, lnk_path, error);
    link->Release();
    if (!saved) {
        return false;
    }
    const std::wstring detail = L"Created shortcut '" + lnk_path + L"' pointing at '" + target + L"' with icon '" + icon_path + L"'.";
    log_event(L"info", L"shortcut", detail.c_str());
    return true;
}

bool retarget_shortcut(const std::wstring& lnk_path, const std::wstring& new_target, const std::wstring& icon_path, std::wstring& error) {
    error.clear();
    if (file_name_is_steam(new_target)) {
        error = L"Refusing to point '" + lnk_path + L"' at steam.exe. The shortcut target is DoomNet.exe, and the icon stays Steam's.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    if (lnk_path.empty() || new_target.empty() || lnk_path.find(L"..") != std::wstring::npos) {
        error = L"Refusing to edit shortcut '" + lnk_path + L"' because a path is empty or contains '..'.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    ComScope com;
    if (!start_com(com, error)) {
        return false;
    }
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        error = L"Could not open a shortcut object for '" + lnk_path + L"'. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (FAILED(hr) || !file) {
        link->Release();
        error = L"Could not load shortcut '" + lnk_path + L"'.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    hr = file->Load(lnk_path.c_str(), STGM_READWRITE);
    if (FAILED(hr)) {
        file->Release();
        link->Release();
        error = L"Could not load shortcut '" + lnk_path + L"'. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    link->SetPath(new_target.c_str());
    if (!icon_path.empty()) {
        link->SetIconLocation(icon_path.c_str(), 0);
    }
    hr = file->Save(lnk_path.c_str(), TRUE);
    file->Release();
    link->Release();
    if (FAILED(hr)) {
        error = L"Could not save shortcut '" + lnk_path + L"' after retarget. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    const std::wstring detail = L"Retargeted '" + lnk_path + L"'. The program is now '" + new_target + L"'. The icon stays '" + icon_path + L"'.";
    log_event(L"info", L"shortcut", detail.c_str());
    return true;
}

bool read_shortcut(const std::wstring& lnk_path, std::wstring& target, std::wstring& icon_path, std::wstring& error) {
    target.clear();
    icon_path.clear();
    error.clear();
    ComScope com;
    if (!start_com(com, error)) {
        return false;
    }
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        error = L"Could not read shortcut '" + lnk_path + L"'. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (FAILED(hr) || !file) {
        link->Release();
        error = L"Could not read shortcut '" + lnk_path + L"'.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    hr = file->Load(lnk_path.c_str(), STGM_READ);
    file->Release();
    if (FAILED(hr)) {
        link->Release();
        error = L"Could not load shortcut '" + lnk_path + L"' for reading. " + windows_error_text(static_cast<unsigned long>(hr));
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    hr = link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH);
    if (FAILED(hr)) {
        link->Release();
        error = L"Could not read the target of shortcut '" + lnk_path + L"'.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    target = path;
    wchar_t icon[MAX_PATH] = {};
    int index = 0;
    hr = link->GetIconLocation(icon, MAX_PATH, &index);
    link->Release();
    if (FAILED(hr)) {
        error = L"Could not read the icon of shortcut '" + lnk_path + L"'.";
        log_event(L"error", L"shortcut", error.c_str());
        return false;
    }
    icon_path = icon;
    return true;
}

}  // namespace doomnet
