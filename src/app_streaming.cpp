/**
 * @file src/app_streaming.cpp
 * @brief Provider-neutral app-window streaming session orchestration.
 */

// standard includes
#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <mutex>
#include <set>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <csignal>
  #include <unistd.h>
#endif

// local includes
#include "app_streaming.h"
#include "config.h"
#include "logging.h"

using namespace std::literals;

namespace app_streaming {
  namespace {
    struct state_t {
      std::filesystem::path state_file;
      std::optional<session_t> pending_session;
      std::optional<session_t> active_session;
      std::mutex mutex;
    };

    state_t APP_STREAMING_STATE;

    struct persistent_state_t {
      std::uint32_t owner_process_id {};
      std::set<std::string> outputs;
    };

    std::unique_ptr<virtual_display_provider_t> provider() {
      return make_virtual_display_provider();
    }

    std::uint32_t current_process_id() {
#ifdef _WIN32
      return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
      return static_cast<std::uint32_t>(getpid());
#endif
    }

    bool process_is_running(std::uint32_t process_id) {
      if (process_id == 0) {
        return false;
      }
#ifdef _WIN32
      auto handle = OpenProcess(SYNCHRONIZE, FALSE, process_id);
      if (!handle) {
        return false;
      }
      const auto result = WaitForSingleObject(handle, 0);
      CloseHandle(handle);
      return result == WAIT_TIMEOUT;
#else
      return kill(static_cast<pid_t>(process_id), 0) == 0;
#endif
    }

    persistent_state_t read_persistent_state() {
      persistent_state_t state;
      if (APP_STREAMING_STATE.state_file.empty()) {
        return state;
      }

      std::ifstream in {APP_STREAMING_STATE.state_file};
      std::string line;
      while (std::getline(in, line)) {
        if (line.empty()) {
          continue;
        }

        if (line.starts_with("owner_pid="sv)) {
          try {
            state.owner_process_id = static_cast<std::uint32_t>(std::stoul(line.substr("owner_pid="sv.size())));
          } catch (...) {
            state.owner_process_id = 0;
          }
        } else if (line.starts_with("output="sv)) {
          state.outputs.emplace(line.substr("output="sv.size()));
        } else {
          state.outputs.emplace(std::move(line));
        }
      }

      return state;
    }

    void write_persistent_state(const persistent_state_t &state) {
      if (APP_STREAMING_STATE.state_file.empty()) {
        return;
      }

      std::error_code ec;
      std::filesystem::create_directories(APP_STREAMING_STATE.state_file.parent_path(), ec);

      std::ofstream out {APP_STREAMING_STATE.state_file, std::ios::trunc};
      if (!state.outputs.empty()) {
        out << "owner_pid="sv << state.owner_process_id << '\n';
      }
      for (const auto &output : state.outputs) {
        out << "output="sv << output << '\n';
      }
    }

    void remember_owned_output(const std::string &output_name) {
      auto state = read_persistent_state();
      state.owner_process_id = current_process_id();
      state.outputs.emplace(output_name);
      write_persistent_state(state);
    }

    void forget_owned_output(const std::string &output_name) {
      auto state = read_persistent_state();
      state.outputs.erase(output_name);
      if (state.outputs.empty()) {
        state.owner_process_id = 0;
      }
      write_persistent_state(state);
    }

    void release_session(const session_t &session) {
      auto p = provider();
      std::string error;
      if (p && !p->release(session.output_name, error)) {
        BOOST_LOG(warning) << "Failed to release app-streaming display ["sv << session.output_name << "]: "sv << error;
      }
      forget_owned_output(session.output_name);
    }

    status_t unsupported_status() {
      status_t result;
      result.supported = false;
      result.provider = "none";
      result.issues.emplace_back("App streaming virtual-display sessions are only supported on Windows.");
      return result;
    }
  }  // namespace

#ifndef _WIN32
  std::unique_ptr<virtual_display_provider_t> make_virtual_display_provider() {
    return nullptr;
  }
#endif

  void init(const std::filesystem::path &state_file) {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    APP_STREAMING_STATE.state_file = state_file;
  }

  void startup_cleanup() {
    std::string error;
    if (!cleanup_owned_displays(error) && !error.empty()) {
      BOOST_LOG(warning) << "App-streaming startup cleanup skipped: "sv << error;
    }
  }

  status_t status() {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};

    auto p = provider();
    auto result = p ? p->status() : unsupported_status();
    result.active_session = APP_STREAMING_STATE.active_session.has_value();
    if (!config::app_streaming.enabled) {
      result.issues.emplace_back("App streaming is disabled in Sunshine configuration.");
    }
    if (config::app_streaming.provider != "sudovda"sv) {
      result.issues.emplace_back("Unsupported app-streaming provider configured: "s + config::app_streaming.provider);
    }
    if (APP_STREAMING_STATE.active_session) {
      result.active_output_name = APP_STREAMING_STATE.active_session->output_name;
    }

    const auto persistent_state = read_persistent_state();
    result.owner_process_id = persistent_state.owner_process_id;
    result.owned_outputs.assign(persistent_state.outputs.begin(), persistent_state.outputs.end());
    return result;
  }

  std::optional<session_t> prepare_session(const session_request_t &request, std::string &error) {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};

    if (APP_STREAMING_STATE.pending_session || APP_STREAMING_STATE.active_session) {
      error = "An app-streaming virtual display is already reserved.";
      return std::nullopt;
    }

    if (!config::app_streaming.enabled) {
      error = "App streaming is disabled in Sunshine configuration.";
      return std::nullopt;
    }

    if (config::app_streaming.provider != "sudovda"sv) {
      error = "Unsupported app-streaming provider configured: " + config::app_streaming.provider;
      return std::nullopt;
    }

    auto p = provider();
    if (!p) {
      error = "App streaming virtual-display sessions are only supported on Windows.";
      return std::nullopt;
    }

    auto provider_status = p->status();
    if (provider_status.foreign_provider_detected) {
      error = "A foreign or stale virtual display provider is present. Cleanup is required before app streaming can start.";
      return std::nullopt;
    }

    if (!provider_status.provider_available) {
      error = "SudoVDA driver is not available for Sunshine-managed app streaming.";
      return std::nullopt;
    }

    auto session = p->reserve(request.mode, error);
    if (!session) {
      return std::nullopt;
    }

    APP_STREAMING_STATE.pending_session = *session;
    remember_owned_output(session->output_name);
    BOOST_LOG(info) << "Reserved app-streaming display ["sv << session->output_name << "] at "sv
                    << session->mode.width << 'x' << session->mode.height << '@' << session->mode.fps;
    return session;
  }

  bool attach_prepared_session(std::uint32_t root_process_id, const session_request_t &request, std::string &error) {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    if (!APP_STREAMING_STATE.pending_session) {
      error = "No prepared app-streaming display is available.";
      return false;
    }

    auto p = provider();
    if (!p) {
      error = "App streaming virtual-display sessions are only supported on Windows.";
      return false;
    }

    return p->attach_window(APP_STREAMING_STATE.pending_session->output_name, root_process_id, request, error);
  }

  bool commit_prepared_session(std::string &error) {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    if (!APP_STREAMING_STATE.pending_session) {
      error = "No prepared app-streaming display is available.";
      return false;
    }

    APP_STREAMING_STATE.active_session = APP_STREAMING_STATE.pending_session;
    APP_STREAMING_STATE.pending_session.reset();
    return true;
  }

  bool update_active_session_mode(const display_mode_t &mode, std::string &error) {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    if (!APP_STREAMING_STATE.active_session) {
      error = "No active app-streaming display is available.";
      return false;
    }

    auto p = provider();
    if (!p) {
      error = "App streaming virtual-display sessions are only supported on Windows.";
      return false;
    }

    if (!p->set_mode(APP_STREAMING_STATE.active_session->output_name, mode, error)) {
      return false;
    }

    APP_STREAMING_STATE.active_session->mode = mode;
    BOOST_LOG(info) << "Updated active app-streaming display ["sv << APP_STREAMING_STATE.active_session->output_name
                    << "] to "sv << mode.width << 'x' << mode.height << '@' << mode.fps;
    return true;
  }

  void cancel_prepared_session() {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    if (!APP_STREAMING_STATE.pending_session) {
      return;
    }

    release_session(*APP_STREAMING_STATE.pending_session);
    APP_STREAMING_STATE.pending_session.reset();
  }

  void end_session() {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    if (APP_STREAMING_STATE.pending_session) {
      release_session(*APP_STREAMING_STATE.pending_session);
      APP_STREAMING_STATE.pending_session.reset();
    }
    if (APP_STREAMING_STATE.active_session) {
      release_session(*APP_STREAMING_STATE.active_session);
      APP_STREAMING_STATE.active_session.reset();
    }
  }

  bool cleanup_owned_displays(std::string &error, bool force) {
    std::lock_guard lock {APP_STREAMING_STATE.mutex};
    if (APP_STREAMING_STATE.pending_session || APP_STREAMING_STATE.active_session) {
      error = "Cannot run app-streaming cleanup while an app-streaming session is active.";
      return false;
    }

    auto state = read_persistent_state();
    if (state.outputs.empty()) {
      return true;
    }

    if (!force && state.owner_process_id != 0 && state.owner_process_id != current_process_id() && process_is_running(state.owner_process_id)) {
      error = std::format("Cannot cleanup app-streaming displays owned by running Sunshine process [{}].", state.owner_process_id);
      return false;
    }

    auto p = provider();
    if (!p) {
      error = "No app-streaming virtual-display provider is available for cleanup.";
      return false;
    }

    bool ok = true;
    for (const auto &output : state.outputs) {
      std::string release_error;
      if (!p->release(output, release_error)) {
        BOOST_LOG(warning) << "Failed to cleanup app-streaming display ["sv << output << "]: "sv << release_error;
        ok = false;
        if (error.empty()) {
          error = release_error;
        }
      }
    }

    if (ok) {
      write_persistent_state({});
    }
    return ok;
  }

  std::vector<discovered_app_t> discover_launchable_apps() {
    if (!config::app_streaming.discover_start_menu) {
      return {};
    }
    auto p = provider();
    return p ? p->discover_launchable_apps() : std::vector<discovered_app_t> {};
  }
}  // namespace app_streaming
