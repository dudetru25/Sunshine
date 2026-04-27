/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <dxgi1_2.h>
#include <roapi.h>
#include <DispatcherQueue.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Common WGC initialization shared between monitor and window capture.
   * Called after `item` has been acquired via CreateForMonitor or CreateForWindow.
   */
  int wgc_capture_t::init_common(display_base_t *display, const ::video::config_t &config) {
    BOOST_LOG(info) << "[WinCap] init_common() starting";
    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      BOOST_LOG(info) << "[WinCap] Creating frame pool (format="sv << display->capture_format << ", size="sv << item.Size().Width << 'x' << item.Size().Height << ')';
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      BOOST_LOG(info) << "[WinCap] Frame pool created, creating capture session...";
      capture_session = frame_pool.CreateCaptureSession(item);
      BOOST_LOG(info) << "[WinCap] Capture session created, registering FrameArrived...";
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
      BOOST_LOG(info) << "[WinCap] FrameArrived registered";
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "[WinCap] Failed to create WGC capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    } catch (std::exception &e) {
      BOOST_LOG(error) << "[WinCap] std::exception in capture session creation: "sv << e.what();
      return -1;
    } catch (...) {
      BOOST_LOG(error) << "[WinCap] Unknown exception in capture session creation";
      return -1;
    }

    if (target_hwnd) {
      item.Closed({this, &wgc_capture_t::on_item_closed});
    }

    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);  // 250Hz
      } else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend for monitor capture.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (monitor_factory == nullptr ||
        FAILED(status = monitor_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    return init_common(display, config);
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend for window capture.
   * @param display The display backend providing D3D11 device context.
   * @param hwnd The window handle to capture.
   * @param config Stream configuration.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, HWND hwnd, const ::video::config_t &config) {
    BOOST_LOG(info) << "[WinCap] wgc_capture_t::init(window) starting";

    HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    BOOST_LOG(info) << "[WinCap] CoInitializeEx result: 0x"sv << util::hex(co_hr).to_string_view();

    // Initialize WinRT runtime explicitly (required for MinGW in service context)
    HRESULT ro_hr = RoInitialize(RO_INIT_MULTITHREADED);
    BOOST_LOG(info) << "[WinCap] RoInitialize result: 0x"sv << util::hex(ro_hr).to_string_view();

    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      // Skip IsSupported() -- it uses IGraphicsCaptureSessionStatics which
      // throws bad_alloc on MinGW builds. We know WGC is available (Win10 1903+).
      BOOST_LOG(info) << "[WinCap] Querying IID_IDXGIDevice from display->device...";
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "[WinCap] Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      BOOST_LOG(info) << "[WinCap] DXGI interface acquired";
      BOOST_LOG(info) << "[WinCap] Calling CreateDirect3D11DeviceFromDXGIDevice...";
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "[WinCap] Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      BOOST_LOG(info) << "[WinCap] WinRT Direct3D device created";
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "[WinCap] WinRT exception acquiring device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    } catch (std::exception &e) {
      BOOST_LOG(error) << "[WinCap] std::exception acquiring device: "sv << e.what();
      return -1;
    } catch (...) {
      BOOST_LOG(error) << "[WinCap] Unknown exception acquiring device";
      return -1;
    }

    try {
      uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
      BOOST_LOG(info) << "[WinCap] IDirect3DDevice acquired";
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "[WinCap] Failed to get IDirect3DDevice: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    } catch (...) {
      BOOST_LOG(error) << "[WinCap] Unknown exception getting IDirect3DDevice";
      return -1;
    }

    target_hwnd = hwnd;

    // WGC CreateForWindow requires a DispatcherQueue on the calling thread
    // to communicate with DWM. Without it, E_OUTOFMEMORY is returned.
    DispatcherQueueOptions dqOptions = {};
    dqOptions.dwSize = sizeof(DispatcherQueueOptions);
    dqOptions.threadType = DQTYPE_THREAD_CURRENT;
    dqOptions.apartmentType = DQTAT_COM_NONE;

    ABI::Windows::System::IDispatcherQueueController *dqController = nullptr;
    HRESULT dq_hr = CreateDispatcherQueueController(dqOptions, &dqController);
    BOOST_LOG(info) << "[WinCap] CreateDispatcherQueueController result: 0x"sv << util::hex(dq_hr).to_string_view();

    try {
      BOOST_LOG(info) << "[WinCap] Getting IGraphicsCaptureItemInterop factory...";
      auto interop_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      if (interop_factory == nullptr) {
        BOOST_LOG(error) << "[WinCap] IGraphicsCaptureItemInterop factory is null";
        return -1;
      }
      BOOST_LOG(info) << "[WinCap] Calling CreateForWindow...";
      status = interop_factory->CreateForWindow(hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item));
      if (FAILED(status)) {
        BOOST_LOG(error) << "[WinCap] CreateForWindow failed: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      BOOST_LOG(info) << "[WinCap] CreateForWindow succeeded";
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "[WinCap] WinRT exception in CreateForWindow: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    } catch (std::exception &e) {
      BOOST_LOG(error) << "[WinCap] std::exception in CreateForWindow: "sv << e.what();
      return -1;
    } catch (...) {
      BOOST_LOG(error) << "[WinCap] Unknown exception in CreateForWindow";
      return -1;
    }

    BOOST_LOG(info) << "[WinCap] Calling init_common...";
    return init_common(display, config);
  }

  void wgc_capture_t::on_item_closed(winrt::GraphicsCaptureItem const &, winrt::IInspectable const &) {
    BOOST_LOG(info) << "Captured window was closed"sv;
    window_closed.store(true);
    WakeConditionVariable(&frame_present_cv);
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    if (window_closed.load()) {
      return capture_e::error;
    }

    if (target_hwnd && IsIconic(target_hwnd)) {
      return capture_e::timeout;
    }

    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);

      if (window_closed.load()) {
        return capture_e::error;
      }

      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
