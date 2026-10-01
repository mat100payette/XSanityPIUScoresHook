#pragma once
#include "ui.h"
#include <wincodec.h>

inline void save_window_preview(
    HWND window, const piu::fs::path& path, bool client_only = false, bool full_content = false) {
    RECT rect{};
    if (client_only) {
        GetClientRect(window, &rect);
    } else {
        GetWindowRect(window, &rect);
    }

    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC screen = client_only ? GetDC(window) : GetWindowDC(window);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    if (full_content) {
        PrintWindow(window, memory, (client_only ? PW_CLIENTONLY : 0) | 2);
    } else if (client_only) {
        // Paint the client and children directly; WM_PRINT's legacy frame is not the DWM frame.
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) {
                continue;
            }

            RECT bounds{};
            GetWindowRect(child, &bounds);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
            int saved = SaveDC(memory);
            SetWindowOrgEx(memory, -bounds.left, -bounds.top, nullptr);
            IntersectClipRect(memory, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top);
            SendMessageW(
                child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
            RestoreDC(memory, saved);
        }
    } else {
        SendMessageW(window,
            WM_PRINT,
            reinterpret_cast<WPARAM>(memory),
            PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    }

    auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
    winrt::com_ptr<IWICBitmap> image;
    winrt::check_hresult(
        factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, image.put()));
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    winrt::check_hresult(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    winrt::check_hresult(frame->WriteSource(image.get(), nullptr));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
}
