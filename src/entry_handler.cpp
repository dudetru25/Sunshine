/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <csignal>
#include <format>
#include <iostream>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "config_schema.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "module_registry.h"
#include "network.h"
#include "platform/common.h"

extern "C" {
#ifdef _WIN32
  #include <iphlpapi.h>
#endif
}

using namespace std::literals;

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

namespace args {
  namespace {
    bool has_arg(int argc, char *argv[], std::string_view arg) {
      for (int x = 0; x < argc; ++x) {
        if (std::string_view {argv[x]} == arg) {
          return true;
        }
      }
      return false;
    }

    void print_json(const nlohmann::json &value) {
      std::cout << value.dump(2) << std::endl;
    }

    int print_action_result(const ::modules::action_result_t &result, bool json_output) {
      if (json_output) {
        auto output = result.data.is_object() ? result.data : nlohmann::json::object();
        output["status"] = result.status;
        if (!result.error.empty()) {
          output["error"] = result.error;
        }
        print_json(output);
      } else if (!result.status) {
        std::cerr << result.error << std::endl;
      } else if (result.data.contains("app_streaming")) {
        const auto &status = result.data["app_streaming"];
        std::cout << "App streaming: " << (status.value("enabled", false) ? "enabled" : "disabled") << std::endl;
        std::cout << "Provider: " << status.value("provider", "none") << std::endl;
        std::cout << "Provider available: " << (status.value("provider_available", false) ? "yes" : "no") << std::endl;
        std::cout << "Foreign provider detected: " << (status.value("foreign_provider_detected", false) ? "yes" : "no") << std::endl;
        std::cout << "Active session: " << (status.value("active_session", false) ? "yes" : "no") << std::endl;
        if (status.contains("issues")) {
          for (const auto &issue : status["issues"]) {
            std::cout << "Issue: " << issue.get<std::string>() << std::endl;
          }
        }
      } else {
        std::cout << "OK" << std::endl;
      }

      return result.status ? 0 : 1;
    }

    nlohmann::json current_config_file_json() {
      nlohmann::json values = nlohmann::json::object();
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (auto &[key, value] : vars) {
        values[key] = value;
      }
      return values;
    }

    void print_config_as_conf(const nlohmann::json &values) {
      for (const auto &definition : config_schema::options()) {
        if (!values.contains(definition.key)) {
          continue;
        }
        std::cout << definition.key << " = " << config_schema::value_to_config_string(values[definition.key]) << std::endl;
      }
    }

    nlohmann::json single_setting_patch(std::string_view key, std::string_view value) {
      nlohmann::json patch = nlohmann::json::object();
      patch[std::string {key}] = std::string {value};
      return patch;
    }
  }  // namespace

  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || std::string_view {argv[0]} == "help"sv || std::string_view {argv[1]} == "help"sv) {
      help(name);
    }

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

  int modules(const char *name, int argc, char *argv[]) {
    const bool json_output = has_arg(argc, argv, "--json");
    auto module_list = ::modules::list_json();
    if (json_output) {
      print_json({{"status", true}, {"modules", module_list}});
      return 0;
    }

    for (const auto &module : module_list) {
      std::cout << module.value("id", "") << "\t" << module.value("name", "") << "\t"
                << (module.value("enabled", false) ? "enabled" : "disabled") << std::endl;
    }
    return 0;
  }

  int module(const char *name, int argc, char *argv[]) {
    const bool json_output = has_arg(argc, argv, "--json");
    const bool force = has_arg(argc, argv, "--force");
    if (argc < 2 || std::string_view {argv[0]} == "help"sv) {
      std::cout << "Usage: " << name << " --module <module_id> <status|settings|actions|doctor|action> [action_id] [--json] [--force]" << std::endl;
      return argc < 2 ? 1 : 0;
    }

    const std::string module_id = argv[0];
    const std::string command = argv[1];
    if (!::modules::exists(module_id)) {
      std::cerr << "Unknown module: " << module_id << std::endl;
      return 1;
    }

    if (command == "status"sv) {
      return print_action_result(::modules::invoke_action(module_id, "status"), json_output);
    }
    if (command == "settings"sv) {
      auto settings = ::modules::settings_json(module_id);
      if (json_output) {
        print_json({{"status", true}, {"module", module_id}, {"settings", settings}});
      } else {
        print_config_as_conf(settings["values"]);
      }
      return 0;
    }
    if (command == "actions"sv) {
      auto actions = ::modules::actions_json(module_id);
      if (json_output) {
        print_json({{"status", true}, {"module", module_id}, {"actions", actions}});
      } else {
        for (const auto &action : actions) {
          std::cout << action.value("id", "") << "\t" << action.value("description", "") << std::endl;
        }
      }
      return 0;
    }
    if (command == "doctor"sv) {
      return print_action_result(::modules::invoke_action(module_id, "doctor"), json_output);
    }
    if (command == "action"sv && argc >= 3) {
      return print_action_result(::modules::invoke_action(module_id, argv[2], {}, force), json_output);
    }

    std::cerr << "Unknown module command: " << command << std::endl;
    return 1;
  }

  int app_streaming_status(const char *name, int argc, char *argv[]) {
    return print_action_result(::modules::invoke_action("app_streaming", "status"), has_arg(argc, argv, "--json"));
  }

  int app_streaming_cleanup(const char *name, int argc, char *argv[]) {
    return print_action_result(::modules::invoke_action("app_streaming", "cleanup", {}, has_arg(argc, argv, "--force")), has_arg(argc, argv, "--json"));
  }

  int app_streaming_discover(const char *name, int argc, char *argv[]) {
    const auto result = ::modules::invoke_action("app_streaming", "discover");
    if (has_arg(argc, argv, "--json")) {
      return print_action_result(result, true);
    }

    if (!result.status) {
      std::cerr << result.error << std::endl;
      return 1;
    }
    for (const auto &app : result.data["apps"]) {
      std::cout << app.value("name", "") << "\t" << app.value("cmd", "") << std::endl;
    }
    return 0;
  }

  int app_streaming_doctor(const char *name, int argc, char *argv[]) {
    const auto result = ::modules::invoke_action("app_streaming", "doctor");
    if (has_arg(argc, argv, "--json")) {
      return print_action_result(result, true);
    }
    if (!result.status) {
      std::cerr << result.error << std::endl;
      return 1;
    }
    print_action_result(::modules::invoke_action("app_streaming", "status"), false);
    for (const auto &diagnostic : result.data["diagnostics"]) {
      std::cout << "Diagnostic: " << diagnostic.get<std::string>() << std::endl;
    }
    return 0;
  }

  int config_cmd(const char *name, int argc, char *argv[]) {
    const bool json_output = has_arg(argc, argv, "--json");
    const bool explicit_output = has_arg(argc, argv, "--explicit");

    if (argc < 1 || std::string_view {argv[0]} == "help"sv) {
      std::cout << "Usage: " << name << " --config <list|get|set|unset|export|validate> [key] [value] [--explicit] [--json|--conf]" << std::endl;
      return argc < 1 ? 1 : 0;
    }

    const std::string command = argv[0];
    if (command == "list"sv || command == "export"sv) {
      const auto values = explicit_output || command == "export"sv ? config_schema::effective_config_json() : current_config_file_json();
      if (json_output) {
        print_json({{"status", true}, {"config", values}});
      } else {
        print_config_as_conf(values);
      }
      return 0;
    }

    if (command == "get"sv && argc >= 2) {
      const std::string key = argv[1];
      if (!config_schema::is_known_key(key)) {
        std::cerr << "Unknown configurable option: " << key << std::endl;
        return 1;
      }
      auto values = config_schema::effective_config_json();
      if (json_output) {
        print_json({{"status", true}, {"key", key}, {"value", values[key]}});
      } else {
        std::cout << config_schema::value_to_config_string(values[key]) << std::endl;
      }
      return 0;
    }

    if (command == "set"sv && argc >= 3) {
      std::string error;
      if (!config_schema::patch_config(single_setting_patch(argv[1], argv[2]), true, error)) {
        std::cerr << error << std::endl;
        return 1;
      }
      if (json_output) {
        print_json({{"status", true}});
      }
      return 0;
    }

    if (command == "unset"sv && argc >= 2) {
      nlohmann::json patch = nlohmann::json::object();
      patch[argv[1]] = nullptr;
      std::string error;
      if (!config_schema::patch_config(patch, true, error)) {
        std::cerr << error << std::endl;
        return 1;
      }
      if (json_output) {
        print_json({{"status", true}});
      }
      return 0;
    }

    if (command == "validate"sv) {
      std::vector<std::string> errors;
      auto values = current_config_file_json();
      const bool ok = config_schema::validate_config(values, errors);
      if (json_output) {
        print_json({{"status", ok}, {"errors", errors}});
      } else if (ok) {
        std::cout << "Config is valid" << std::endl;
      } else {
        for (const auto &error : errors) {
          std::cerr << error << std::endl;
        }
      }
      return ok ? 0 : 1;
    }

    std::cerr << "Unknown or incomplete config command: " << command << std::endl;
    return 1;
  }

#ifdef _WIN32
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif
}  // namespace args

namespace lifetime {
  char **argv;
  std::atomic_int desired_exit_code;

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  class service_controller {
  public:
    /**
     * @brief Constructor for service_controller class.
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      service_handle = OpenServiceA(scm_handle, "SunshineService", service_desired_access);
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << std::format("{} failed to start: {}"sv, platf::SERVICE_NAME, status.dwWin32ExitCode);
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
