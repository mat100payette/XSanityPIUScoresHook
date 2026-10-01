#include "screenshots.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wincodec.h>
#include <chrono>

namespace piu {
namespace {
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

std::string encode_png(
    ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const RECT& crop) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    auto width = static_cast<UINT>(crop.right - crop.left);
    auto height = static_cast<UINT>(crop.bottom - crop.top);
    if (!width || !height || crop.left < 0 || crop.top < 0 || static_cast<UINT>(crop.right) > desc.Width ||
        static_cast<UINT>(crop.bottom) > desc.Height) {
        throw Error("Game window changed size during capture.");
    }
    desc.Width = width;
    desc.Height = height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
    D3D11_BOX box{static_cast<UINT>(crop.left),
        static_cast<UINT>(crop.top),
        0,
        static_cast<UINT>(crop.right),
        static_cast<UINT>(crop.bottom),
        1};
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));

    struct Unmap {
        ID3D11DeviceContext* context;
        ID3D11Texture2D* texture;

        ~Unmap() {
            context->Unmap(texture, 0);
        }
    } unmap{context, staging.get()};

    auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
    winrt::com_ptr<IStream> stream;
    winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    winrt::check_hresult(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) {
        throw Error("PNG pixel format unavailable.");
    }
    winrt::check_hresult(frame->WritePixels(
        height, mapped.RowPitch, mapped.RowPitch * height, static_cast<BYTE*>(mapped.pData)));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
    STATSTG stat{};
    winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > 64 * 1024 * 1024) {
        throw Error("Screenshot too large.");
    }
    std::string png(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
    LARGE_INTEGER start{};
    winrt::check_hresult(stream->Seek(start, STREAM_SEEK_SET, nullptr));
    ULONG received = 0;
    winrt::check_hresult(stream->Read(png.data(), static_cast<ULONG>(png.size()), &received));
    if (received != png.size()) {
        throw Error("Incomplete PNG.");
    }
    return png;
}

HWND find_game_window(const fs::path& root) {
    struct Search {
        fs::path root;
        HWND window = nullptr;
        LONG area = 0;
    } search{root};

    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto& found = *reinterpret_cast<Search*>(parameter);
            if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER)) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            if (!process.get()) {
                return TRUE;
            }
            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) {
                return TRUE;
            }
            path.resize(length);
            std::error_code ignored;
            bool match = fs::equivalent(path, found.root / L"Program64/XSanity.exe", ignored);
            if (!match) {
                match = fs::equivalent(path, found.root / L"Program32/XSanity.exe", ignored);
            }
            if (!match) {
                return TRUE;
            }
            RECT client{};
            GetClientRect(window, &client);
            auto area = client.right * client.bottom;
            if (area > found.area) {
                found.window = window;
                found.area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.window) {
        throw Error("No visible XSanity game window.");
    }
    return search.window;
}
} // namespace

std::string capture_window_png(HWND window) {
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GraphicsCaptureSession::IsSupported()) {
        throw Error("Window capture unavailable.");
    }
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        device.put(),
        nullptr,
        context.put()));
    auto dxgi = device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    auto direct = inspectable.as<IDirect3DDevice>();
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(
        interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0 || size.Width > 8192 || size.Height > 8192) {
        throw Error("Unsupported game window size.");
    }
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        direct, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    auto session = pool.CreateCaptureSession(item);

    struct CloseCapture {
        GraphicsCaptureSession session;
        Direct3D11CaptureFramePool pool;

        ~CloseCapture() {
            session.Close();
            pool.Close();
        }
    } close{session, pool};

    session.StartCapture();
    auto until = GetTickCount64() + 1500;
    do {
        if (auto frame = pool.TryGetNextFrame()) {
            auto content = frame.ContentSize();
            if (content.Width != size.Width || content.Height != size.Height) {
                throw Error("Window resized during capture.");
            }
            RECT bounds{}, client{};
            winrt::check_hresult(
                DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)));
            GetClientRect(window, &client);
            POINT origin{0, 0};
            ClientToScreen(window, &origin);
            RECT crop{origin.x - bounds.left,
                origin.y - bounds.top,
                origin.x - bounds.left + client.right,
                origin.y - bounds.top + client.bottom};
            auto access =
                frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
            return encode_png(device.get(), context.get(), texture.get(), crop);
        }
        Sleep(15);
    } while (GetTickCount64() < until);
    throw Error("Timed out waiting for the game frame.");
}

std::string capture_game_png(const fs::path& game_root) {
    return capture_window_png(find_game_window(game_root));
}
} // namespace piu
