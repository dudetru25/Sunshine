/**
 * @file src/platform/windows/app_streaming.cpp
 * @brief Windows SudoVDA app-streaming provider.
 */

// standard includes
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

// lib includes
#include <boost/algorithm/string.hpp>

// clang-format off
#include <Windows.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <WinUser.h>
// clang-format on

// local includes
#include "src/app_streaming.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/utf_utils.h"
#include "src/utility.h"

using namespace std::literals;

namespace app_streaming {
  namespace {
    constexpr DWORD IOCTL_ADD_VIRTUAL_DISPLAY = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_REMOVE_VIRTUAL_DISPLAY = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_GET_WATCHDOG = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_DRIVER_PING = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_GET_PROTOCOL_VERSION = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS);

    // {e5bcc234-1e0c-418a-a0d4-ef8b7501414d}
    constexpr GUID SUDOVDA_INTERFACE_GUID = {0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};

    // Fixed identity for Sunshine's module-owned app-streaming display.
    // {85cc5606-0f76-46a6-bf97-d36967412b66}
    constexpr GUID SUNSHINE_APP_STREAMING_DISPLAY_GUID = {0x85cc5606, 0x0f76, 0x46a6, {0xbf, 0x97, 0xd3, 0x69, 0x67, 0x41, 0x2b, 0x66}};

    struct sudovda_protocol_version_t {
      std::uint8_t major;
      std::uint8_t minor;
      std::uint8_t incremental;
      bool test_build;
    };

    constexpr sudovda_protocol_version_t SUNSHINE_SUDOVDA_PROTOCOL_VERSION {0, 2, 1, true};

    struct virtual_display_add_params_t {
      UINT width;
      UINT height;
      UINT refresh_rate;
      GUID monitor_guid;
      CHAR device_name[14];
      CHAR serial_number[14];
    };

    struct virtual_display_remove_params_t {
      GUID monitor_guid;
    };

    struct virtual_display_add_out_t {
      LUID adapter_luid;
      UINT target_id;
    };

    struct virtual_display_get_watchdog_out_t {
      UINT timeout;
      UINT countdown;
    };

    struct virtual_display_get_protocol_version_out_t {
      sudovda_protocol_version_t version;
    };

    struct driver_session_state_t {
      ~driver_session_state_t() {
        stop_ping = true;
        if (ping_thread.joinable()) {
          ping_thread.join();
        }
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
          handle = INVALID_HANDLE_VALUE;
        }
      }

      std::mutex mutex;
      HANDLE handle {INVALID_HANDLE_VALUE};
      std::thread ping_thread;
      std::atomic_bool stop_ping {false};
      std::string output_name;
    };

    driver_session_state_t SUDOVDA_SESSION;

    struct display_device_t {
      std::wstring device_name;
      std::string text;
      bool active {};
      bool sudo_vda {};
      bool foreign_vdd {};
    };

    struct monitor_t {
      std::wstring device_name;
      RECT rect {};
      bool primary {};
      bool sudo_vda {};
      bool foreign_vdd {};
    };

    struct window_t {
      HWND hwnd {};
      DWORD pid {};
      std::string title;
      std::string process_name;
      RECT rect {};
      int score {};
    };

    std::string lower_copy(std::string value) {
      boost::algorithm::to_lower(value);
      return value;
    }

    std::string normalized_sudovda_label(std::string value, std::string_view fallback) {
      boost::algorithm::trim(value);
      if (value.empty()) {
        value = fallback;
      }

      value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch > 0x7E;
      }), value.end());

      if (value.empty()) {
        value = fallback;
      }
      if (value.size() > 13) {
        value.resize(13);
      }
      return value;
    }

    void copy_sudovda_label(CHAR (&destination)[14], const std::string &value) {
      std::fill(std::begin(destination), std::end(destination), '\0');
      std::copy_n(value.data(), std::min<std::size_t>(value.size(), 13), destination);
    }

    std::string win32_error_string(std::string_view operation, DWORD error_code) {
      return std::format("{} failed with Win32 error {}", operation, error_code);
    }

    bool sudovda_protocol_compatible(const sudovda_protocol_version_t &driver_version) {
      if (driver_version.major != SUNSHINE_SUDOVDA_PROTOCOL_VERSION.major) {
        return false;
      }

      return SUNSHINE_SUDOVDA_PROTOCOL_VERSION.minor <= driver_version.minor;
    }

    std::string quote_for_cmd_start(const std::filesystem::path &path) {
      auto value = path.string();
      boost::replace_all(value, "\"", "\\\"");
      return "cmd /C start \"\" \""s + value + '"';
    }

    bool text_matches_sudo_vda(const std::string &text) {
      auto lower = lower_copy(text);
      return lower.find("sudomaker"sv) != std::string::npos ||
             lower.find("sudovda"sv) != std::string::npos;
    }

    bool text_matches_foreign_vdd(const std::string &text) {
      auto lower = lower_copy(text);
      if (text_matches_sudo_vda(lower)) {
        return false;
      }

      return lower.find("mtt1337"sv) != std::string::npos ||
             lower.find("mikethetech"sv) != std::string::npos ||
             lower.find("mtt"sv) != std::string::npos ||
             lower.find("parsec virtual"sv) != std::string::npos ||
             lower.find("virtual display driver"sv) != std::string::npos;
    }

    display_device_t make_display_device(const DISPLAY_DEVICEW &device) {
      auto text = utf_utils::to_utf8(device.DeviceName) + " " +
                  utf_utils::to_utf8(device.DeviceString) + " " +
                  utf_utils::to_utf8(device.DeviceID) + " " +
                  utf_utils::to_utf8(device.DeviceKey);

      return display_device_t {
        device.DeviceName,
        text,
        (device.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0,
        text_matches_sudo_vda(text),
        text_matches_foreign_vdd(text),
      };
    }

    std::vector<display_device_t> enumerate_display_devices() {
      std::vector<display_device_t> devices;
      for (DWORD index = 0; index < 64; ++index) {
        DISPLAY_DEVICEW adapter {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
          break;
        }

        devices.emplace_back(make_display_device(adapter));

        for (DWORD monitor_index = 0; monitor_index < 16; ++monitor_index) {
          DISPLAY_DEVICEW monitor {};
          monitor.cb = sizeof(monitor);
          if (!EnumDisplayDevicesW(adapter.DeviceName, monitor_index, &monitor, 0)) {
            break;
          }

          devices.emplace_back(make_display_device(monitor));
        }
      }

      return devices;
    }

    bool display_name_matches(const std::wstring &left, const std::wstring &right) {
      return boost::iequals(utf_utils::to_utf8(left), utf_utils::to_utf8(right));
    }

    display_device_t device_info_for_display_name(const std::wstring &device_name) {
      display_device_t result;
      result.device_name = device_name;

      for (const auto &device : enumerate_display_devices()) {
        if (display_name_matches(device.device_name, device_name)) {
          result.active = result.active || device.active;
          result.sudo_vda = result.sudo_vda || device.sudo_vda;
          result.foreign_vdd = result.foreign_vdd || device.foreign_vdd;
          result.text += " " + device.text;
        }
      }

      return result;
    }

    std::vector<monitor_t> enumerate_monitors() {
      std::vector<monitor_t> monitors;
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM lparam) -> BOOL {
        MONITORINFOEXW info {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
          return TRUE;
        }

        auto device = device_info_for_display_name(info.szDevice);
        auto *results = reinterpret_cast<std::vector<monitor_t> *>(lparam);
        results->push_back(monitor_t {
          info.szDevice,
          info.rcMonitor,
          (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
          device.sudo_vda,
          device.foreign_vdd,
        });
        return TRUE;
      }, reinterpret_cast<LPARAM>(&monitors));

      return monitors;
    }

    std::optional<monitor_t> find_monitor(const std::string &output_name) {
      auto output_name_w = utf_utils::from_utf8(output_name);
      auto monitors = enumerate_monitors();
      auto match = std::find_if(monitors.begin(), monitors.end(), [&](const auto &monitor) {
        return display_name_matches(monitor.device_name, output_name_w);
      });

      if (match == monitors.end()) {
        return std::nullopt;
      }

      return *match;
    }

    bool has_foreign_virtual_display(std::vector<std::string> &issues) {
      bool detected = false;
      for (const auto &device : enumerate_display_devices()) {
        if (!device.foreign_vdd) {
          continue;
        }

        detected = true;
        issues.emplace_back("Foreign virtual display provider detected: " + device.text);
      }
      return detected;
    }

    bool query_current_mode(const std::wstring &device_name, DEVMODEW &mode, std::string &error) {
      mode = {};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        error = "Failed to query current display mode for " + utf_utils::to_utf8(device_name);
        return false;
      }
      return true;
    }

    bool set_display_mode_exact(const std::wstring &device_name, const display_mode_t &mode_request, std::string &error) {
      if (mode_request.width <= 0 || mode_request.height <= 0 || mode_request.fps <= 0) {
        error = "Invalid app-streaming display mode.";
        return false;
      }

      DEVMODEW mode {};
      if (!query_current_mode(device_name, mode, error)) {
        return false;
      }

      if ((int) mode.dmPelsWidth == mode_request.width &&
          (int) mode.dmPelsHeight == mode_request.height &&
          (int) mode.dmDisplayFrequency == mode_request.fps) {
        return true;
      }

      BOOST_LOG(info) << "Changing SudoVDA display "sv << utf_utils::to_utf8(device_name)
                      << " from "sv << mode.dmPelsWidth << 'x' << mode.dmPelsHeight << '@' << mode.dmDisplayFrequency
                      << " to "sv << mode_request.width << 'x' << mode_request.height << '@' << mode_request.fps;

      mode.dmPelsWidth = mode_request.width;
      mode.dmPelsHeight = mode_request.height;
      mode.dmDisplayFrequency = mode_request.fps;
      mode.dmBitsPerPel = 32;
      mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;

      auto result = ChangeDisplaySettingsExW(device_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
      if (result != DISP_CHANGE_SUCCESSFUL) {
        error = "Failed to set exact SudoVDA mode for " + utf_utils::to_utf8(device_name) + " (ChangeDisplaySettingsExW=" + std::to_string(result) + ")";
        return false;
      }

      std::this_thread::sleep_for(500ms);

      DEVMODEW after {};
      if (!query_current_mode(device_name, after, error)) {
        return false;
      }

      if ((int) after.dmPelsWidth != mode_request.width ||
          (int) after.dmPelsHeight != mode_request.height ||
          (int) after.dmDisplayFrequency != mode_request.fps) {
        std::ostringstream ss;
        ss << "SudoVDA did not apply exact mode. Requested "
           << mode_request.width << 'x' << mode_request.height << '@' << mode_request.fps
           << ", got " << after.dmPelsWidth << 'x' << after.dmPelsHeight << '@' << after.dmDisplayFrequency;
        error = ss.str();
        return false;
      }

      return true;
    }

    bool detach_display(const std::wstring &device_name, std::string &error) {
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
      mode.dmPosition.x = 0;
      mode.dmPosition.y = 0;
      mode.dmPelsWidth = 0;
      mode.dmPelsHeight = 0;

      auto result = ChangeDisplaySettingsExW(device_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
      if (result != DISP_CHANGE_SUCCESSFUL) {
        error = "Failed to stage SudoVDA detach for " + utf_utils::to_utf8(device_name) + " (ChangeDisplaySettingsExW=" + std::to_string(result) + ")";
        return false;
      }

      result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
      if (result != DISP_CHANGE_SUCCESSFUL) {
        error = "Failed to apply SudoVDA detach for " + utf_utils::to_utf8(device_name) + " (ChangeDisplaySettingsExW=" + std::to_string(result) + ")";
        return false;
      }

      std::this_thread::sleep_for(500ms);
      return true;
    }

    HANDLE open_sudovda_device(std::string &error) {
      auto device_info_set = SetupDiGetClassDevsA(&SUDOVDA_INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      if (device_info_set == INVALID_HANDLE_VALUE) {
        error = win32_error_string("SetupDiGetClassDevsA for SudoVDA", GetLastError());
        return INVALID_HANDLE_VALUE;
      }

      auto cleanup = util::fail_guard([&device_info_set]() {
        SetupDiDestroyDeviceInfoList(device_info_set);
      });

      DWORD last_error = ERROR_FILE_NOT_FOUND;
      for (DWORD index = 0; index < 32; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data {};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(device_info_set, nullptr, &SUDOVDA_INTERFACE_GUID, index, &interface_data)) {
          last_error = GetLastError();
          break;
        }

        DWORD detail_size = 0;
        SetupDiGetDeviceInterfaceDetailA(device_info_set, &interface_data, nullptr, 0, &detail_size, nullptr);
        if (detail_size == 0) {
          last_error = GetLastError();
          continue;
        }

        std::vector<std::uint8_t> detail_buffer(detail_size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(device_info_set, &interface_data, detail, detail_size, &detail_size, nullptr)) {
          last_error = GetLastError();
          continue;
        }

        auto handle = CreateFileA(
          detail->DevicePath,
          GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL,
          nullptr
        );

        if (handle != INVALID_HANDLE_VALUE) {
          return handle;
        }

        last_error = GetLastError();
      }

      error = win32_error_string("Opening SudoVDA driver interface", last_error);
      return INVALID_HANDLE_VALUE;
    }

    void close_handle_if_valid(HANDLE &handle) {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
      }
    }

    bool get_sudovda_protocol_version(HANDLE handle, sudovda_protocol_version_t &version, std::string &error) {
      virtual_display_get_protocol_version_out_t output {};
      DWORD bytes_returned {};
      if (!DeviceIoControl(handle, IOCTL_GET_PROTOCOL_VERSION, nullptr, 0, &output, sizeof(output), &bytes_returned, nullptr)) {
        error = win32_error_string("SudoVDA protocol query", GetLastError());
        return false;
      }

      version = output.version;
      return true;
    }

    bool check_sudovda_protocol(HANDLE handle, std::string &error) {
      sudovda_protocol_version_t driver_version {};
      if (!get_sudovda_protocol_version(handle, driver_version, error)) {
        return false;
      }

      if (!sudovda_protocol_compatible(driver_version)) {
        error = std::format(
          "SudoVDA protocol is incompatible. Sunshine requires {}.{}, driver returned {}.{}.",
          static_cast<int>(SUNSHINE_SUDOVDA_PROTOCOL_VERSION.major),
          static_cast<int>(SUNSHINE_SUDOVDA_PROTOCOL_VERSION.minor),
          static_cast<int>(driver_version.major),
          static_cast<int>(driver_version.minor)
        );
        return false;
      }

      return true;
    }

    bool probe_sudovda_driver(std::string &error) {
      auto handle = open_sudovda_device(error);
      if (handle == INVALID_HANDLE_VALUE) {
        return false;
      }

      auto cleanup = util::fail_guard([&handle]() {
        close_handle_if_valid(handle);
      });

      return check_sudovda_protocol(handle, error);
    }

    bool add_virtual_display(HANDLE handle, const display_mode_t &mode, virtual_display_add_out_t &output, std::string &error) {
      const auto device_name = normalized_sudovda_label(config::app_streaming.sudovda_device_name, "SunshineVDD"sv);
      const auto serial = normalized_sudovda_label(config::app_streaming.sudovda_serial, "Sunshine0001"sv);

      virtual_display_add_params_t params {
        static_cast<UINT>(mode.width),
        static_cast<UINT>(mode.height),
        static_cast<UINT>(mode.fps * 1000),
        SUNSHINE_APP_STREAMING_DISPLAY_GUID,
        {},
        {},
      };
      copy_sudovda_label(params.device_name, device_name);
      copy_sudovda_label(params.serial_number, serial);

      DWORD bytes_returned {};
      if (!DeviceIoControl(handle, IOCTL_ADD_VIRTUAL_DISPLAY, &params, sizeof(params), &output, sizeof(output), &bytes_returned, nullptr)) {
        error = win32_error_string("SudoVDA AddVirtualDisplay", GetLastError());
        return false;
      }

      return true;
    }

    bool remove_virtual_display(HANDLE handle, std::string &error) {
      virtual_display_remove_params_t params {SUNSHINE_APP_STREAMING_DISPLAY_GUID};

      DWORD bytes_returned {};
      if (!DeviceIoControl(handle, IOCTL_REMOVE_VIRTUAL_DISPLAY, &params, sizeof(params), nullptr, 0, &bytes_returned, nullptr)) {
        error = win32_error_string("SudoVDA RemoveVirtualDisplay", GetLastError());
        return false;
      }

      return true;
    }

    bool ping_sudovda_driver(HANDLE handle) {
      DWORD bytes_returned {};
      return DeviceIoControl(handle, IOCTL_DRIVER_PING, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
    }

    std::optional<virtual_display_get_watchdog_out_t> get_sudovda_watchdog(HANDLE handle) {
      virtual_display_get_watchdog_out_t output {};
      DWORD bytes_returned {};
      if (!DeviceIoControl(handle, IOCTL_GET_WATCHDOG, nullptr, 0, &output, sizeof(output), &bytes_returned, nullptr)) {
        return std::nullopt;
      }
      return output;
    }

    bool get_added_display_name(const virtual_display_add_out_t &added_display, wchar_t *device_name) {
      UINT path_count {};
      UINT mode_count {};
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return false;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
      }

      auto path = std::find_if(paths.begin(), paths.end(), [&](const DISPLAYCONFIG_PATH_INFO &candidate) {
        return candidate.targetInfo.id == added_display.target_id;
      });
      if (path == paths.end()) {
        return false;
      }

      DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
      source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source_name.header.size = sizeof(source_name);
      source_name.header.adapterId = added_display.adapter_luid;
      source_name.header.id = path->sourceInfo.id;

      if (DisplayConfigGetDeviceInfo(reinterpret_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER *>(&source_name)) != ERROR_SUCCESS) {
        return false;
      }

      wcsncpy(device_name, source_name.viewGdiDeviceName, CCHDEVICENAME - 1);
      device_name[CCHDEVICENAME - 1] = L'\0';
      return true;
    }

    std::optional<std::wstring> wait_for_added_display_name(const virtual_display_add_out_t &added_display) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (std::chrono::steady_clock::now() < deadline) {
        wchar_t device_name[CCHDEVICENAME] {};
        if (get_added_display_name(added_display, device_name)) {
          return std::wstring {device_name};
        }

        std::this_thread::sleep_for(100ms);
      }

      return std::nullopt;
    }

    void stop_ping_thread_locked() {
      SUDOVDA_SESSION.stop_ping = true;
      if (SUDOVDA_SESSION.ping_thread.joinable()) {
        SUDOVDA_SESSION.ping_thread.join();
      }
      SUDOVDA_SESSION.stop_ping = false;
    }

    void start_ping_thread_locked(HANDLE handle) {
      stop_ping_thread_locked();

      auto watchdog = get_sudovda_watchdog(handle);
      if (!watchdog || watchdog->timeout == 0) {
        return;
      }

      const auto interval = std::chrono::milliseconds {std::max<UINT>(watchdog->timeout * 1000 / 3, 1000)};
      BOOST_LOG(info) << "SudoVDA watchdog: timeout="sv << watchdog->timeout
                      << " countdown="sv << watchdog->countdown
                      << " ping_interval_ms="sv << interval.count();

      SUDOVDA_SESSION.ping_thread = std::thread([handle, interval]() {
        std::uint8_t failures = 0;
        while (!SUDOVDA_SESSION.stop_ping.load()) {
          std::this_thread::sleep_for(interval);
          if (SUDOVDA_SESSION.stop_ping.load()) {
            break;
          }

          if (ping_sudovda_driver(handle)) {
            failures = 0;
            continue;
          }

          ++failures;
          BOOST_LOG(warning) << "SudoVDA watchdog ping failed: "sv << GetLastError();
          if (failures > 3) {
            BOOST_LOG(error) << "SudoVDA watchdog ping failed repeatedly; the driver may remove the virtual display."sv;
            break;
          }
        }
      });
    }

    bool remove_module_display_with_handle(HANDLE handle, std::string &error) {
      std::string remove_error;
      if (remove_virtual_display(handle, remove_error)) {
        return true;
      }

      error = remove_error;
      return false;
    }

    bool release_created_sudovda_display(const std::string &output_name, std::string &error) {
      std::lock_guard lock {SUDOVDA_SESSION.mutex};

      if (SUDOVDA_SESSION.handle != INVALID_HANDLE_VALUE) {
        stop_ping_thread_locked();
        bool removed = remove_module_display_with_handle(SUDOVDA_SESSION.handle, error);
        if (!removed && !output_name.empty() && !find_monitor(output_name)) {
          error.clear();
          removed = true;
        }
        close_handle_if_valid(SUDOVDA_SESSION.handle);
        SUDOVDA_SESSION.output_name.clear();
        return removed;
      }

      auto handle = open_sudovda_device(error);
      if (handle == INVALID_HANDLE_VALUE) {
        auto monitor = output_name.empty() ? std::nullopt : find_monitor(output_name);
        if (!monitor) {
          error.clear();
          return true;
        }
        return false;
      }

      auto cleanup = util::fail_guard([&handle]() {
        close_handle_if_valid(handle);
      });

      if (remove_module_display_with_handle(handle, error)) {
        return true;
      }

      auto monitor = output_name.empty() ? std::nullopt : find_monitor(output_name);
      if (!monitor) {
        error.clear();
        return true;
      }

      if (!monitor->sudo_vda || monitor->primary) {
        error = "Refusing to detach a non-SudoVDA or primary display: " + output_name;
        return false;
      }

      BOOST_LOG(warning) << "Could not remove module-owned SudoVDA display by GUID; detaching active output fallback: "sv << output_name;
      return detach_display(monitor->device_name, error);
    }

    std::optional<std::wstring> create_sudovda_display(const display_mode_t &mode, std::string &error) {
      std::lock_guard lock {SUDOVDA_SESSION.mutex};

      if (SUDOVDA_SESSION.handle != INVALID_HANDLE_VALUE) {
        error = "A SudoVDA display is already owned by this Sunshine process.";
        return std::nullopt;
      }

      auto handle = open_sudovda_device(error);
      if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
      }

      auto cleanup = util::fail_guard([&handle]() {
        close_handle_if_valid(handle);
      });

      if (!check_sudovda_protocol(handle, error)) {
        return std::nullopt;
      }

      virtual_display_add_out_t added_display {};
      if (!add_virtual_display(handle, mode, added_display, error)) {
        BOOST_LOG(warning) << "Initial SudoVDA display creation failed; attempting stale Sunshine display cleanup before retry: "sv << error;

        std::string cleanup_error;
        remove_virtual_display(handle, cleanup_error);

        if (!add_virtual_display(handle, mode, added_display, error)) {
          return std::nullopt;
        }
      }

      auto output_name = wait_for_added_display_name(added_display);
      if (!output_name) {
        std::string release_error;
        remove_virtual_display(handle, release_error);
        error = "SudoVDA created a display but Windows did not expose an active GDI display name.";
        return std::nullopt;
      }

      start_ping_thread_locked(handle);
      SUDOVDA_SESSION.output_name = utf_utils::to_utf8(*output_name);
      SUDOVDA_SESSION.handle = handle;
      handle = INVALID_HANDLE_VALUE;

      BOOST_LOG(info) << "Created module-owned SudoVDA display: "sv << SUDOVDA_SESSION.output_name;
      return output_name;
    }

    std::string process_name_for_pid(DWORD pid) {
      std::string exe_name;
      HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!proc) {
        return exe_name;
      }

      WCHAR path[MAX_PATH] = {};
      DWORD path_size = MAX_PATH;
      if (QueryFullProcessImageNameW(proc, 0, path, &path_size)) {
        std::wstring wpath(path, path_size);
        auto slash = wpath.find_last_of(L'\\');
        auto wname = slash == std::wstring::npos ? wpath : wpath.substr(slash + 1);
        exe_name = utf_utils::to_utf8(wname);
      }

      CloseHandle(proc);
      return exe_name;
    }

    std::set<DWORD> process_tree_pids(DWORD root_pid) {
      std::set<DWORD> ids;
      if (root_pid == 0) {
        return ids;
      }

      ids.emplace(root_pid);
      bool changed = true;
      while (changed) {
        changed = false;

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
          break;
        }

        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
          do {
            if (ids.count(entry.th32ParentProcessID) && !ids.count(entry.th32ProcessID)) {
              ids.emplace(entry.th32ProcessID);
              changed = true;
            }
          } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
      }

      return ids;
    }

    struct window_search_context_t {
      const std::set<DWORD> *pid_tree {};
      std::string match_lower;
      bool require_pid_tree_match {};
      std::vector<window_t> candidates;
    };

    BOOL CALLBACK enum_app_windows(HWND hwnd, LPARAM lparam) {
      if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
      }

      int title_len = GetWindowTextLengthW(hwnd);
      if (title_len <= 0) {
        return TRUE;
      }

      RECT rect {};
      if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
      }

      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width < 80 || height < 60) {
        return TRUE;
      }

      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid == 0) {
        return TRUE;
      }

      std::wstring title_w(title_len + 1, L'\0');
      GetWindowTextW(hwnd, title_w.data(), title_len + 1);
      title_w.resize(title_len);

      auto title = utf_utils::to_utf8(title_w);
      auto process_name = process_name_for_pid(pid);
      auto *ctx = reinterpret_cast<window_search_context_t *>(lparam);

      bool in_pid_tree = ctx->pid_tree && ctx->pid_tree->count(pid);
      if (ctx->require_pid_tree_match && !in_pid_tree) {
        return TRUE;
      }

      auto title_lower = lower_copy(title);
      auto process_lower = lower_copy(process_name);
      if (process_lower == "unitybugreporter.exe"sv || title_lower.find("bug reporter"sv) != std::string::npos) {
        return TRUE;
      }

      int score = 0;
      if (in_pid_tree) {
        score += 1000;
      }

      if (!ctx->match_lower.empty()) {
        bool title_match = title_lower.find(ctx->match_lower) != std::string::npos;
        bool process_match = process_lower.find(ctx->match_lower) != std::string::npos;
        if (ctx->require_pid_tree_match && !title_match && !process_match) {
          return TRUE;
        }

        if (title_match) {
          score += 300;
        }
        if (process_match) {
          score += 500;
        }
      }

      if (score > 0) {
        score += std::min<int>((width * height) / 100000, 200);
        ctx->candidates.push_back(window_t {
          hwnd,
          pid,
          std::move(title),
          std::move(process_name),
          rect,
          score,
        });
      }

      return TRUE;
    }

    std::vector<window_t> collect_app_windows(std::uint32_t root_process_id, const std::string &match_hint, bool require_pid_tree_match) {
      auto pid_tree = process_tree_pids(root_process_id);
      window_search_context_t ctx {
        &pid_tree,
        lower_copy(match_hint),
        require_pid_tree_match,
        {},
      };

      EnumWindows(enum_app_windows, reinterpret_cast<LPARAM>(&ctx));
      std::sort(ctx.candidates.begin(), ctx.candidates.end(), [](const auto &left, const auto &right) {
        return left.score > right.score;
      });
      return ctx.candidates;
    }

    std::optional<window_t> find_app_window(std::uint32_t root_process_id, const std::string &match_hint, std::chrono::seconds timeout) {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      bool require_pid_tree_match = root_process_id != 0;

      while (std::chrono::steady_clock::now() < deadline) {
        auto candidates = collect_app_windows(root_process_id, match_hint, require_pid_tree_match);
        if (!candidates.empty()) {
          return candidates.front();
        }

        BOOST_LOG(info) << "Waiting for app-streaming window matching ["sv << match_hint << "]"sv;
        std::this_thread::sleep_for(1s);
      }

      return std::nullopt;
    }

    bool make_window_borderless(HWND hwnd, const monitor_t &monitor, std::string &error) {
      LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

      LONG_PTR new_style = style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
      LONG_PTR new_ex_style = ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

      SetLastError(ERROR_SUCCESS);
      if (new_style != style && SetWindowLongPtrW(hwnd, GWL_STYLE, new_style) == 0 && GetLastError() != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Failed to clear app window style for app streaming: "sv << GetLastError();
      }

      SetLastError(ERROR_SUCCESS);
      if (new_ex_style != ex_style && SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_ex_style) == 0 && GetLastError() != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Failed to clear app window extended style for app streaming: "sv << GetLastError();
      }

      auto width = monitor.rect.right - monitor.rect.left;
      auto height = monitor.rect.bottom - monitor.rect.top;
      if (!SetWindowPos(hwnd, nullptr, monitor.rect.left, monitor.rect.top, width, height, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW)) {
        error = "SetWindowPos failed while applying borderless app-streaming bounds: " + std::to_string(GetLastError());
        return false;
      }

      return true;
    }

    bool move_window_to_monitor(HWND hwnd, const monitor_t &monitor, bool borderless, std::string &error) {
      ShowWindow(hwnd, SW_RESTORE);
      std::this_thread::sleep_for(200ms);

      auto width = monitor.rect.right - monitor.rect.left;
      auto height = monitor.rect.bottom - monitor.rect.top;
      if (!SetWindowPos(hwnd, nullptr, monitor.rect.left, monitor.rect.top, width, height, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
        error = "SetWindowPos failed while moving app to SudoVDA display: " + std::to_string(GetLastError());
        return false;
      }

      if (borderless && !make_window_borderless(hwnd, monitor, error)) {
        return false;
      }

      SetForegroundWindow(hwnd);
      return true;
    }

    bool window_fills_monitor(HWND hwnd, const monitor_t &monitor) {
      RECT rect {};
      if (!GetWindowRect(hwnd, &rect)) {
        return false;
      }

      return rect.left == monitor.rect.left &&
             rect.top == monitor.rect.top &&
             rect.right == monitor.rect.right &&
             rect.bottom == monitor.rect.bottom;
    }

    void start_window_follower(std::uint32_t root_process_id, const std::string &match_hint, const std::string &output_name, bool borderless, std::chrono::seconds timeout) {
      std::thread([root_process_id, match_hint, output_name, borderless, timeout]() {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::set<HWND> handled_windows;
        bool require_pid_tree_match = root_process_id != 0;

        while (std::chrono::steady_clock::now() < deadline) {
          auto monitor = find_monitor(output_name);
          if (!monitor) {
            break;
          }

          auto candidates = collect_app_windows(root_process_id, match_hint, require_pid_tree_match);
          for (auto &candidate : candidates) {
            if (!IsWindow(candidate.hwnd) || handled_windows.count(candidate.hwnd) || window_fills_monitor(candidate.hwnd, *monitor)) {
              continue;
            }

            std::string error;
            if (move_window_to_monitor(candidate.hwnd, *monitor, borderless, error)) {
              handled_windows.emplace(candidate.hwnd);
            } else {
              BOOST_LOG(warning) << "App-streaming follower failed to move window: "sv << error;
            }
          }

          std::this_thread::sleep_for(1s);
        }
      }).detach();
    }

    std::vector<std::filesystem::path> start_menu_roots() {
      std::vector<std::filesystem::path> roots;

      WCHAR program_data[MAX_PATH] = {};
      if (GetEnvironmentVariableW(L"ProgramData", program_data, _countof(program_data)) > 0) {
        roots.emplace_back(std::filesystem::path {program_data} / L"Microsoft\\Windows\\Start Menu\\Programs");
      }

      WCHAR app_data[MAX_PATH] = {};
      if (GetEnvironmentVariableW(L"APPDATA", app_data, _countof(app_data)) > 0) {
        roots.emplace_back(std::filesystem::path {app_data} / L"Microsoft\\Windows\\Start Menu\\Programs");
      }

      return roots;
    }

    class sudo_vda_provider_t: public virtual_display_provider_t {
    public:
      std::string name() const override {
        return "sudovda";
      }

      status_t status() override {
        status_t result;
        result.supported = true;
        result.provider = name();
        result.foreign_provider_detected = has_foreign_virtual_display(result.issues);

        std::string driver_error;
        result.provider_available = probe_sudovda_driver(driver_error);
        if (!result.provider_available) {
          result.issues.emplace_back("SudoVDA driver interface is not available: " + driver_error);
        }

        for (const auto &monitor : enumerate_monitors()) {
          if (monitor.sudo_vda && monitor.primary) {
            result.issues.emplace_back("SudoVDA is currently primary; app streaming requires a non-primary virtual display.");
          }
        }

        return result;
      }

      std::optional<session_t> reserve(const display_mode_t &mode, std::string &error) override {
        auto created_output = create_sudovda_display(mode, error);
        if (!created_output) {
          return std::nullopt;
        }

        const auto created_output_name = utf_utils::to_utf8(*created_output);

        auto monitors = enumerate_monitors();
        for (const auto &monitor : monitors) {
          BOOST_LOG(info) << "App-streaming display candidate: "sv
                          << utf_utils::to_utf8(monitor.device_name)
                          << " primary="sv << (monitor.primary ? "true"sv : "false"sv)
                          << " sudovda="sv << (monitor.sudo_vda ? "true"sv : "false"sv)
                          << " foreign="sv << (monitor.foreign_vdd ? "true"sv : "false"sv)
                          << " rect=("sv << monitor.rect.left << ',' << monitor.rect.top
                          << ' ' << (monitor.rect.right - monitor.rect.left)
                          << 'x' << (monitor.rect.bottom - monitor.rect.top) << ')';
        }

        auto selected = std::find_if(monitors.begin(), monitors.end(), [&](const auto &monitor) {
          return monitor.sudo_vda && display_name_matches(monitor.device_name, *created_output);
        });

        if (selected == monitors.end()) {
          release_created_sudovda_display(created_output_name, error);
          error = "SudoVDA display was created but could not be matched to an active monitor.";
          return std::nullopt;
        }

        if (selected->primary) {
          release_created_sudovda_display(created_output_name, error);
          error = "Refusing to stream from a SudoVDA display that Windows marked as primary.";
          return std::nullopt;
        }

        if (!set_display_mode_exact(selected->device_name, mode, error)) {
          std::string release_error;
          release_created_sudovda_display(created_output_name, release_error);
          return std::nullopt;
        }

        return session_t {
          created_output_name,
          mode,
        };
      }

      bool set_mode(const std::string &output_name, const display_mode_t &mode, std::string &error) override {
        auto monitor = find_monitor(output_name);
        if (!monitor) {
          error = "Active SudoVDA display disappeared: " + output_name;
          return false;
        }
        if (!monitor->sudo_vda || monitor->primary) {
          error = "Refusing to change mode for a non-SudoVDA or primary display: " + output_name;
          return false;
        }

        return set_display_mode_exact(monitor->device_name, mode, error);
      }

      bool attach_window(const std::string &output_name, std::uint32_t root_process_id, const session_request_t &request, std::string &error) override {
        auto monitor = find_monitor(output_name);
        if (!monitor) {
          error = "Prepared SudoVDA display disappeared: " + output_name;
          return false;
        }
        if (!monitor->sudo_vda || monitor->primary) {
          error = "Refusing to move app to a non-SudoVDA or primary display: " + output_name;
          return false;
        }

        auto match_hint = request.window_match.empty() ? request.app_name : request.window_match;
        auto window = find_app_window(root_process_id, match_hint, request.window_timeout);
        if (!window) {
          error = "No app window matched [" + match_hint + "].";
          return false;
        }

        BOOST_LOG(info) << "App-streaming window resolved: title=["sv << window->title
                        << "] process=["sv << window->process_name
                        << "] pid="sv << window->pid
                        << " output=["sv << output_name << ']';

        if (!move_window_to_monitor(window->hwnd, *monitor, request.borderless, error)) {
          return false;
        }

        if (request.follow_windows) {
          start_window_follower(root_process_id, match_hint, output_name, request.borderless, request.window_follow_timeout);
        }

        return true;
      }

      bool release(const std::string &output_name, std::string &error) override {
        BOOST_LOG(info) << "Releasing module-owned SudoVDA app-streaming display: "sv << output_name;
        return release_created_sudovda_display(output_name, error);
      }

      std::vector<discovered_app_t> discover_launchable_apps() override {
        std::vector<discovered_app_t> apps;
        std::set<std::string> seen;

        for (const auto &root : start_menu_roots()) {
          std::error_code ec;
          if (!std::filesystem::exists(root, ec)) {
            continue;
          }

          for (std::filesystem::recursive_directory_iterator it {root, ec}, end; it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec) || it->path().extension() != L".lnk") {
              continue;
            }

            auto name = it->path().stem().string();
            if (name.empty() || !seen.emplace(lower_copy(name)).second) {
              continue;
            }

            apps.push_back(discovered_app_t {
              name,
              quote_for_cmd_start(it->path()),
              {},
            });
          }
        }

        std::sort(apps.begin(), apps.end(), [](const auto &left, const auto &right) {
          return left.name < right.name;
        });
        return apps;
      }
    };
  }  // namespace

  std::unique_ptr<virtual_display_provider_t> make_virtual_display_provider() {
    return std::make_unique<sudo_vda_provider_t>();
  }
}  // namespace app_streaming
