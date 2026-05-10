/**
 * @file src/module_registry.cpp
 * @brief Built-in Sunshine module registry and action dispatch.
 */

// standard includes
#include <algorithm>
#include <exception>
#include <mutex>

// local includes
#include "app_streaming.h"
#include "config.h"
#include "config_schema.h"
#include "file_handler.h"
#include "module_registry.h"
#include "platform/common.h"

using namespace std::literals;

namespace modules {
  namespace {
    using json = nlohmann::json;

    std::once_flag INIT_FLAG;

    void ensure_initialized() {
      std::call_once(INIT_FLAG, []() {
        app_streaming::init(platf::appdata() / "app_streaming.state");
      });
    }

    json app_streaming_status_json(const app_streaming::status_t &status) {
      json tree;
      tree["supported"] = status.supported;
      tree["enabled"] = config::app_streaming.enabled;
      tree["provider"] = status.provider;
      tree["configured_provider"] = config::app_streaming.provider;
      tree["provider_available"] = status.provider_available;
      tree["foreign_provider_detected"] = status.foreign_provider_detected;
      tree["active_session"] = status.active_session;
      tree["active_output_name"] = status.active_output_name;
      tree["owner_process_id"] = status.owner_process_id;
      tree["issues"] = status.issues;
      tree["owned_outputs"] = status.owned_outputs;
      return tree;
    }

    json app_streaming_actions_json() {
      return json::array({
        {
          {"id", "status"},
          {"description", "Return app-streaming provider and session status."},
          {"mutating", false},
        },
        {
          {"id", "cleanup"},
          {"description", "Release stale module-owned virtual displays."},
          {"mutating", true},
          {"supports_force", true},
        },
        {
          {"id", "discover"},
          {"description", "Discover launchable Windows Start Menu applications."},
          {"mutating", false},
        },
        {
          {"id", "doctor"},
          {"description", "Return app-streaming diagnostics and suggested fixes."},
          {"mutating", false},
        },
      });
    }

    json app_streaming_module_json(bool include_status) {
      json module;
      module["id"] = "app_streaming";
      module["name"] = "App Streaming";
      module["description"] = "Streams individual applications through a managed virtual display provider.";
      module["platform"] = "windows";
      module["enabled"] = config::app_streaming.enabled;
      module["supported"] = app_streaming::status().supported;
      module["settings"] = config_schema::schema_json_for_module("app_streaming");
      module["actions"] = app_streaming_actions_json();
      if (include_status) {
        module["status_detail"] = app_streaming_status_json(app_streaming::status());
      }
      return module;
    }

    bool module_setting_key_matches(std::string_view module_id, std::string_view key) {
      const auto *definition = config_schema::find_option(key);
      return definition && definition->module == module_id;
    }

    action_result_t status_action() {
      action_result_t result;
      result.status = true;
      result.data["app_streaming"] = app_streaming_status_json(app_streaming::status());
      return result;
    }

    action_result_t cleanup_action(bool force) {
      action_result_t result;
      std::string error;
      result.status = app_streaming::cleanup_owned_displays(error, force);
      result.error = error;
      result.data["app_streaming"] = app_streaming_status_json(app_streaming::status());
      return result;
    }

    action_result_t discover_action() {
      action_result_t result;
      result.status = true;
      result.data["apps"] = json::array();
      for (const auto &app : app_streaming::discover_launchable_apps()) {
        result.data["apps"].push_back({
          {"name", app.name},
          {"cmd", app.cmd},
          {"working-dir", app.working_dir},
        });
      }
      return result;
    }

    action_result_t doctor_action() {
      action_result_t result;
      result.status = true;
      auto status = app_streaming::status();
      result.data["app_streaming"] = app_streaming_status_json(status);
      result.data["diagnostics"] = json::array();

      if (!config::app_streaming.enabled) {
        result.data["diagnostics"].push_back("Enable app_streaming_enabled before starting app-streaming sessions.");
      }
      if (config::app_streaming.provider != "sudovda"sv) {
        result.data["diagnostics"].push_back("Set app_streaming_provider to sudovda.");
      }
      if (!status.supported) {
        result.data["diagnostics"].push_back("App streaming is currently supported only on Windows.");
      }
      if (status.foreign_provider_detected) {
        result.data["diagnostics"].push_back("Remove or disable stale foreign virtual display providers before starting app streaming.");
      }
      if (!status.provider_available) {
        result.data["diagnostics"].push_back("Install the SudoVDA driver. Sunshine will create and remove its app-streaming display automatically.");
      }
      if (!status.owned_outputs.empty() && !status.active_session) {
        result.data["diagnostics"].push_back("Run the cleanup action to release stale module-owned virtual displays.");
      }
      return result;
    }

    void apply_current_config() {
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      config::apply_config(std::move(vars));
    }
  }  // namespace

  void init() {
    ensure_initialized();
  }

  bool exists(std::string_view module_id) {
    return module_id == "app_streaming"sv;
  }

  json list_json() {
    ensure_initialized();
    return json::array({app_streaming_module_json(false)});
  }

  json module_json(std::string_view module_id) {
    ensure_initialized();
    if (module_id == "app_streaming"sv) {
      return app_streaming_module_json(true);
    }
    return {};
  }

  json settings_json(std::string_view module_id) {
    ensure_initialized();
    if (!exists(module_id)) {
      return {};
    }

    json output;
    output["schema"] = config_schema::schema_json_for_module(module_id);
    output["values"] = config_schema::effective_config_json_for_module(module_id);
    return output;
  }

  json actions_json(std::string_view module_id) {
    ensure_initialized();
    if (module_id == "app_streaming"sv) {
      return app_streaming_actions_json();
    }
    return {};
  }

  action_result_t invoke_action(std::string_view module_id, std::string_view action, const json &payload, bool force) {
    ensure_initialized();
    if (module_id != "app_streaming"sv) {
      return {false, "Unknown module.", {}};
    }

    force = force || payload.value("force", false);
    if (action == "status"sv) {
      return status_action();
    }
    if (action == "cleanup"sv) {
      return cleanup_action(force);
    }
    if (action == "discover"sv) {
      return discover_action();
    }
    if (action == "doctor"sv) {
      return doctor_action();
    }

    return {false, "Unknown module action.", {}};
  }

  action_result_t update_settings(std::string_view module_id, const json &settings) {
    ensure_initialized();
    if (!exists(module_id)) {
      return {false, "Unknown module.", {}};
    }
    if (!settings.is_object()) {
      return {false, "Module settings payload must be a JSON object.", {}};
    }

    for (const auto &[key, _] : settings.items()) {
      if (!module_setting_key_matches(module_id, key)) {
        return {false, "Setting [" + key + "] does not belong to module [" + std::string {module_id} + "].", {}};
      }
    }

    std::string error;
    if (!config_schema::patch_config(settings, true, error)) {
      return {false, error, {}};
    }

    try {
      apply_current_config();
    } catch (const std::exception &err) {
      return {false, err.what(), {}};
    }

    action_result_t result;
    result.status = true;
    result.data["settings"] = config_schema::effective_config_json_for_module(module_id);
    result.data["restart_required"] = false;
    return result;
  }
}  // namespace modules
