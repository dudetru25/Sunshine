/**
 * @file src/platform/windows/window_utils.cpp
 * @brief Windows implementation of per-window capture utilities.
 *
 * Implements platf::enumerate_windows(), platf::window_display(),
 * platf::focus_window(), and platf::window_stream_to_screen() for Windows.
 */
// standard includes
#include <sstream>
#include <string>
#include <vector>

// platform includes
#include <Windows.h>
#include <Psapi.h>

// local includes
#include "display.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

namespace platf {
  using namespace std::literals;

  struct enum_windows_ctx_t {
    std::vector<window_info_t> results;
    DWORD sunshine_pid;
  };

  static BOOL CALLBACK enum_windows_callback(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<enum_windows_ctx_t *>(lparam);

    if (IsWindowVisible(hwnd) == false) {
      return TRUE;
    }

    if (IsIconic(hwnd)) {
      return TRUE;
    }

    WCHAR title_buf[256] {};
    auto _title_len = GetWindowTextW(hwnd, title_buf, sizeof(title_buf) / sizeof(WCHAR));
    if (_title_len <= 0) {
      return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->sunshine_pid) {
      return TRUE;
    }

    // Skip windows with no real content area
    RECT client_rect {};
    GetClientRect(hwnd, &client_rect);
    auto _client_width = client_rect.right - client_rect.left;
    auto _client_height = client_rect.bottom - client_rect.top;
    if (_client_width <= 0 || _client_height <= 0) {
      return TRUE;
    }

    window_info_t info;

    // Store HWND as hex string
    std::ostringstream ss;
    ss << "0x" << std::hex << reinterpret_cast<uintptr_t>(hwnd);
    info.id = ss.str();

    // Convert wide title to UTF-8
    auto _needed = WideCharToMultiByte(CP_UTF8, 0, title_buf, _title_len, nullptr, 0, nullptr, nullptr);
    info.title.resize(_needed);
    WideCharToMultiByte(CP_UTF8, 0, title_buf, _title_len, info.title.data(), _needed, nullptr, nullptr);

    info.width = _client_width;
    info.height = _client_height;
    info.visible = true;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
      WCHAR exe_buf[MAX_PATH] {};
      DWORD exe_size = MAX_PATH;
      if (QueryFullProcessImageNameW(process, 0, exe_buf, &exe_size)) {
        auto _exe_needed = WideCharToMultiByte(CP_UTF8, 0, exe_buf, exe_size, nullptr, 0, nullptr, nullptr);
        info.exe_path.resize(_exe_needed);
        WideCharToMultiByte(CP_UTF8, 0, exe_buf, exe_size, info.exe_path.data(), _exe_needed, nullptr, nullptr);

        auto _last_sep = info.exe_path.find_last_of("\\/");
        info.exe_name = (_last_sep != std::string::npos) ? info.exe_path.substr(_last_sep + 1) : info.exe_path;
      }
      CloseHandle(process);
    }

    ctx->results.push_back(std::move(info));
    return TRUE;
  }

  std::vector<window_info_t> enumerate_windows() {
    enum_windows_ctx_t ctx;
    ctx.sunshine_pid = GetCurrentProcessId();

    EnumWindows(enum_windows_callback, reinterpret_cast<LPARAM>(&ctx));

    return std::move(ctx.results);
  }

  std::shared_ptr<display_t> window_display(mem_type_e hwdevice_type, const std::string &window_id, const video::config_t &config) {
    if (hwdevice_type == mem_type_e::dxgi) {
      auto disp = std::make_shared<dxgi::display_wgc_vram_t>();
      if (disp->init(config, window_id, true) == 0) {
        return disp;
      }
    } else if (hwdevice_type == mem_type_e::system) {
      auto disp = std::make_shared<dxgi::display_wgc_ram_t>();
      if (disp->init(config, window_id, true) == 0) {
        return disp;
      }
    }

    return nullptr;
  }

  void focus_window(const std::string &window_id) {
    auto hwnd = reinterpret_cast<HWND>(std::stoull(window_id, nullptr, 16));
    if (hwnd == nullptr) {
      return;
    }

    if (GetForegroundWindow() != hwnd) {
      SetForegroundWindow(hwnd);
    }
  }

  util::point_t window_stream_to_screen(const std::string &window_id, float stream_x, float stream_y, int stream_width, int stream_height) {
    auto hwnd = reinterpret_cast<HWND>(std::stoull(window_id, nullptr, 16));

    RECT client_rect {};
    GetClientRect(hwnd, &client_rect);
    auto _client_width = client_rect.right - client_rect.left;
    auto _client_height = client_rect.bottom - client_rect.top;

    POINT origin {0, 0};
    ClientToScreen(hwnd, &origin);

    auto _screen_x = origin.x + (stream_x / stream_width) * _client_width;
    auto _screen_y = origin.y + (stream_y / stream_height) * _client_height;

    return {static_cast<double>(_screen_x), static_cast<double>(_screen_y)};
  }
}  // namespace platf
