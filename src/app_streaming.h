/**
 * @file src/app_streaming.h
 * @brief App-window streaming session orchestration.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app_streaming {
  struct display_mode_t {
    int width {};
    int height {};
    int fps {};
  };

  struct session_request_t {
    std::string app_name;
    std::string window_match;
    display_mode_t mode;
    bool borderless {true};
    bool follow_windows {true};
    std::chrono::seconds window_timeout {60};
    std::chrono::seconds window_follow_timeout {1800};
  };

  struct session_t {
    std::string output_name;
    display_mode_t mode;
  };

  struct status_t {
    bool supported {};
    bool provider_available {};
    bool foreign_provider_detected {};
    bool active_session {};
    std::uint32_t owner_process_id {};
    std::string provider;
    std::string active_output_name;
    std::vector<std::string> issues;
    std::vector<std::string> owned_outputs;
  };

  struct discovered_app_t {
    std::string name;
    std::string cmd;
    std::string working_dir;
  };

  class virtual_display_provider_t {
  public:
    virtual ~virtual_display_provider_t() = default;

    virtual std::string name() const = 0;
    virtual status_t status() = 0;
    virtual std::optional<session_t> reserve(const display_mode_t &mode, std::string &error) = 0;
    virtual bool set_mode(const std::string &output_name, const display_mode_t &mode, std::string &error) = 0;
    virtual bool attach_window(const std::string &output_name, std::uint32_t root_process_id, const session_request_t &request, std::string &error) = 0;
    virtual bool release(const std::string &output_name, std::string &error) = 0;
    virtual std::vector<discovered_app_t> discover_launchable_apps() = 0;
  };

  std::unique_ptr<virtual_display_provider_t> make_virtual_display_provider();

  void init(const std::filesystem::path &state_file);
  void startup_cleanup();
  status_t status();

  std::optional<session_t> prepare_session(const session_request_t &request, std::string &error);
  bool attach_prepared_session(std::uint32_t root_process_id, const session_request_t &request, std::string &error);
  bool commit_prepared_session(std::string &error);
  bool update_active_session_mode(const display_mode_t &mode, std::string &error);
  void cancel_prepared_session();
  void end_session();

  bool cleanup_owned_displays(std::string &error, bool force = false);
  std::vector<discovered_app_t> discover_launchable_apps();
}  // namespace app_streaming
