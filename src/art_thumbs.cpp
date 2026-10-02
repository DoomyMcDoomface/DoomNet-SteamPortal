#include "art_thumbs.h"

#include "host_bus.h"
#include "log.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cwctype>
#include <thread>

namespace doomnet {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring lower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return text;
}

std::wstring hresult_text(HRESULT hr) {
    return windows_error_text(static_cast<unsigned long>(hr));
}

bool file_time(const std::wstring& path, ULONGLONG& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    out = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return true;
}

HRESULT make_factory(ComPtr<IWICImagingFactory>& factory) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
}

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

}  // namespace

std::wstring art_thumb_path(const std::wstring& album_cover) {
    const size_t slash = album_cover.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) {
        return {};
    }
    const std::wstring name = album_cover.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        return {};
    }
    const std::wstring ext = lower(name.substr(dot));
    if (ext != L".jpg" && ext != L".jpeg" && ext != L".png") {
        return {};
    }
    const std::wstring folder = album_cover.substr(0, slash);
    const size_t parent_slash = folder.find_last_of(L"\\/");
    const std::wstring folder_name = lower(parent_slash == std::wstring::npos ? folder : folder.substr(parent_slash + 1));
    if (folder_name != L"albums" || parent_slash == std::wstring::npos) {
        return {};
    }
    return folder.substr(0, parent_slash) + L"\\thumbs\\" + name.substr(0, dot) + L".jpg";
}

bool art_image_size(const std::wstring& path, unsigned& width, unsigned& height) {
    width = 0;
    height = 0;
    ComScope com;
    if (!com.usable()) {
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(make_factory(factory))
        || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(frame->GetSize(&w, &h))) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

bool write_art_thumbnail(const std::wstring& source, const std::wstring& dest, unsigned max_side, std::wstring& error) {
    error.clear();
    ComScope com;
    if (!com.usable()) {
        error = L"COM could not start on this thread, so '" + source + L"' got no thumbnail. " + hresult_text(com.hr);
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = make_factory(factory);
    if (FAILED(hr)) {
        error = L"The Windows image library did not start, so '" + source + L"' got no thumbnail. " + hresult_text(hr);
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, &frame);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&width, &height);
    }
    if (FAILED(hr) || width == 0 || height == 0) {
        error = L"Windows could not read '" + source + L"' as an image. " + hresult_text(hr);
        return false;
    }
    UINT out_w = width;
    UINT out_h = height;
    if (max_side > 0 && (width > max_side || height > max_side)) {
        if (width >= height) {
            out_w = max_side;
            out_h = static_cast<UINT>((static_cast<unsigned long long>(height) * max_side + width / 2) / width);
        } else {
            out_h = max_side;
            out_w = static_cast<UINT>((static_cast<unsigned long long>(width) * max_side + height / 2) / height);
        }
        out_w = out_w == 0 ? 1 : out_w;
        out_h = out_h == 0 ? 1 : out_h;
    }
    ComPtr<IWICBitmapSource> scaled = frame;
    if (out_w != width || out_h != height) {
        ComPtr<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) {
            hr = scaler->Initialize(frame.Get(), out_w, out_h, WICBitmapInterpolationModeFant);
        }
        if (FAILED(hr)) {
            error = L"Windows could not shrink '" + source + L"'. " + hresult_text(hr);
            return false;
        }
        scaled = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(scaled.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        error = L"Windows could not convert '" + source + L"' to plain colour pixels. " + hresult_text(hr);
        return false;
    }

    const std::wstring temp = dest + L".tmp";
    DeleteFileW(temp.c_str());
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE);
    }
    if (FAILED(hr)) {
        error = L"Could not create '" + temp + L"'. " + hresult_text(hr);
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> out_frame;
    ComPtr<IPropertyBag2> props;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&out_frame, &props);
    }
    if (SUCCEEDED(hr) && props) {
        PROPBAG2 option{};
        wchar_t quality_name[] = L"ImageQuality";
        option.pstrName = quality_name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.85f;
        props->Write(1, &option, &value);
    }
    if (SUCCEEDED(hr)) {
        hr = out_frame->Initialize(props.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = out_frame->SetSize(out_w, out_h);
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) {
        hr = out_frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(hr)) {
        hr = out_frame->WriteSource(converter.Get(), nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = out_frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    out_frame.Reset();
    encoder.Reset();
    stream.Reset();
    if (FAILED(hr)) {
        DeleteFileW(temp.c_str());
        error = L"Could not write the thumbnail for '" + source + L"' to '" + temp + L"'. " + hresult_text(hr);
        return false;
    }
    if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        DeleteFileW(temp.c_str());
        error = L"Could not move '" + temp + L"' to '" + dest + L"'. " + windows_error_text(code);
        return false;
    }
    return true;
}

struct ArtThumbs::Impl {
    std::wstring albums;
    std::wstring thumbs;
    HANDLE stop_event = nullptr;
    std::thread thread;
    bool first_pass = true;

    bool stopping() const {
        return WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
    }

    void pass() {
        if (!CreateDirectoryW(thumbs.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            if (first_pass) {
                log_event(L"warn", L"art", (L"Could not create '" + thumbs + L"', so the grids keep loading full-size covers. " + windows_error_text(GetLastError())).c_str());
            }
            return;
        }
        WIN32_FIND_DATAW found{};
        const HANDLE find = FindFirstFileExW((albums + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            if (first_pass) {
                log_event(L"info", L"art", (L"'" + albums + L"' has no covers yet. Thumbnails start when the player publishes some.").c_str());
            }
            return;
        }
        unsigned made = 0;
        unsigned current = 0;
        unsigned failed = 0;
        std::wstring first_error;
        do {
            if (stopping()) {
                break;
            }
            if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                continue;
            }
            const std::wstring source = albums + L"\\" + found.cFileName;
            const std::wstring dest = art_thumb_path(source);
            if (dest.empty()) {
                continue;
            }
            ULONGLONG source_time = 0;
            ULONGLONG dest_time = 0;
            if (!file_time(source, source_time)) {
                continue;
            }
            if (file_time(dest, dest_time) && dest_time >= source_time) {
                ++current;
                continue;
            }
            std::wstring error;
            if (write_art_thumbnail(source, dest, kArtThumbSize, error)) {
                ++made;
            } else {
                ++failed;
                if (first_error.empty()) {
                    first_error = error;
                }
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
        if (made > 0 || failed > 0 || first_pass) {
            std::wstring line = L"Grid thumbnails in '" + thumbs + L"': " + std::to_wstring(made) + L" written, " + std::to_wstring(current) + L" already current, " + std::to_wstring(failed) + L" failed.";
            if (!first_error.empty()) {
                line += L" First failure: " + first_error;
            }
            log_event(failed > 0 ? L"warn" : L"info", L"art", line.c_str());
        }
        first_pass = false;
    }

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        DWORD_PTR lua_mask = 0;
        DWORD_PTR art_mask = 0;
        Bus::cpu_masks(lua_mask, art_mask);
        if (art_mask != 0 && SetThreadAffinityMask(GetCurrentThread(), art_mask) == 0) {
            log_event(L"warn", L"art", (L"Could not pin thumbnail loading to its own CPU. Covers are still built off the Lua thread. " + windows_error_text(GetLastError())).c_str());
        } else if (art_mask != 0) {
            log_event(L"info", L"art", L"Thumbnail loading is running on its own CPU, off the Lua thread.");
        } else {
            log_event(L"info", L"art", L"Thumbnail loading is running off the Lua thread. This machine has one CPU, so it was not pinned to a second one.");
        }
        ComScope com;
        if (!com.usable()) {
            log_event(L"warn", L"art", (L"COM could not start on the thumbnail thread, so the grids keep loading full-size covers from '" + albums + L"'. " + hresult_text(com.hr)).c_str());
            return;
        }
        do {
            pass();
        } while (WaitForSingleObject(stop_event, 60000) == WAIT_TIMEOUT);
    }
};

ArtThumbs::ArtThumbs() : impl_(std::make_unique<Impl>()) {}

ArtThumbs::~ArtThumbs() {
    stop();
}

bool ArtThumbs::start(const std::wstring& albums, std::wstring& error) {
    error.clear();
    if (impl_->thread.joinable()) {
        return true;
    }
    const size_t slash = albums.find_last_of(L"\\/");
    if (slash == std::wstring::npos || lower(albums.substr(slash + 1)) != L"albums") {
        error = L"'" + albums + L"' is not a folder named albums, so no grid thumbnails were made.";
        return false;
    }
    impl_->albums = albums;
    impl_->thumbs = albums.substr(0, slash) + L"\\thumbs";
    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl_->stop_event == nullptr) {
        error = L"Could not create the stop event for the thumbnail thread. " + windows_error_text(GetLastError());
        return false;
    }
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

void ArtThumbs::stop() {
    if (!impl_) {
        return;
    }
    if (impl_->stop_event != nullptr) {
        SetEvent(impl_->stop_event);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->stop_event != nullptr) {
        CloseHandle(impl_->stop_event);
        impl_->stop_event = nullptr;
    }
}

}  // namespace doomnet
