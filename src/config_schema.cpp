/**
 * @file src/config_schema.cpp
 * @brief Registry and validation helpers for Sunshine configuration options.
 */

// standard includes
#include <algorithm>
#include <format>
#include <sstream>

// lib includes
#include <boost/algorithm/string.hpp>

// local includes
#include "config.h"
#include "config_schema.h"
#include "file_handler.h"

using namespace std::literals;

namespace config_schema {
  namespace {
    using json = nlohmann::json;

    option_definition_t opt(
      std::string key,
      option_type_e type,
      json default_value,
      std::string section,
      std::string module = {},
      std::vector<std::string> choices = {},
      std::optional<double> minimum = std::nullopt,
      std::optional<double> maximum = std::nullopt,
      bool runtime_mutable = false,
      bool restart_required = true
    ) {
      return {
        std::move(key),
        type,
        std::move(default_value),
        std::move(section),
        std::move(module),
        std::move(choices),
        minimum,
        maximum,
        runtime_mutable,
        restart_required,
      };
    }

    std::string type_name(option_type_e type) {
      switch (type) {
        case option_type_e::string:
          return "string";
        case option_type_e::integer:
          return "integer";
        case option_type_e::number:
          return "number";
        case option_type_e::boolean:
          return "boolean";
        case option_type_e::array:
          return "array";
        case option_type_e::object:
          return "object";
      }
      return "unknown";
    }

    bool is_bool_string(const std::string &value) {
      auto lower = boost::to_lower_copy(value);
      return lower == "true"sv || lower == "false"sv ||
             lower == "yes"sv || lower == "no"sv ||
             lower == "enable"sv || lower == "disable"sv ||
             lower == "enabled"sv || lower == "disabled"sv ||
             lower == "on"sv || lower == "off"sv ||
             lower == "1"sv || lower == "0"sv;
    }

    std::string join_strings(const std::vector<std::string> &values, std::string_view separator) {
      std::string result;
      for (std::size_t x = 0; x < values.size(); ++x) {
        if (x > 0) {
          result += separator;
        }
        result += values[x];
      }
      return result;
    }

    std::optional<double> parse_number_string(const std::string &raw, bool integer) {
      try {
        std::size_t consumed {};
        auto number = integer ? static_cast<double>(std::stoll(raw, &consumed)) : std::stod(raw, &consumed);
        if (consumed != raw.size()) {
          return std::nullopt;
        }
        return number;
      } catch (...) {
        return std::nullopt;
      }
    }

    bool validate_type(const option_definition_t &definition, const json &value, std::string &error) {
      if (value.is_null()) {
        error = "must not be null";
        return false;
      }

      std::optional<double> numeric_value;
      switch (definition.type) {
        case option_type_e::string:
          if (!(value.is_string() || value.is_number() || value.is_boolean())) {
            error = "must be a string";
            return false;
          }
          break;
        case option_type_e::integer:
          if (!(value.is_number_integer() || value.is_string())) {
            error = "must be an integer";
            return false;
          }
          if (value.is_string()) {
            numeric_value = parse_number_string(value.get<std::string>(), true);
            if (!numeric_value) {
              error = "must be an integer";
              return false;
            }
          } else {
            numeric_value = value.get<double>();
          }
          break;
        case option_type_e::number:
          if (!(value.is_number() || value.is_string())) {
            error = "must be a number";
            return false;
          }
          if (value.is_string()) {
            numeric_value = parse_number_string(value.get<std::string>(), false);
            if (!numeric_value) {
              error = "must be a number";
              return false;
            }
          } else {
            numeric_value = value.get<double>();
          }
          break;
        case option_type_e::boolean:
          if (value.is_boolean()) {
            break;
          }
          if (!value.is_string() || !is_bool_string(value.get<std::string>())) {
            error = "must be a boolean or enabled/disabled string";
            return false;
          }
          break;
        case option_type_e::array:
          if (!(value.is_array() || value.is_string())) {
            error = "must be an array";
            return false;
          }
          break;
        case option_type_e::object:
          if (!(value.is_object() || value.is_string())) {
            error = "must be an object";
            return false;
          }
          break;
      }

      if (!definition.choices.empty() && (value.is_string() || value.is_number() || value.is_boolean())) {
        auto raw = value.is_string() ? value.get<std::string>() : value.dump();
        if (!raw.empty() && std::ranges::find(definition.choices, raw) == definition.choices.end()) {
          error = std::format("must be one of [{}]", join_strings(definition.choices, ", "));
          return false;
        }
      }

      if ((definition.minimum || definition.maximum) && numeric_value) {
        if (definition.minimum && *numeric_value < *definition.minimum) {
          error = std::format("must be greater than or equal to {}", *definition.minimum);
          return false;
        }
        if (definition.maximum && *numeric_value > *definition.maximum) {
          error = std::format("must be less than or equal to {}", *definition.maximum);
          return false;
        }
      }

      return true;
    }

    json parse_value(const option_definition_t &definition, const std::string &raw) {
      if (raw.empty()) {
        return definition.default_value.is_string() ? json(raw) : definition.default_value;
      }

      switch (definition.type) {
        case option_type_e::boolean:
          return raw;
        case option_type_e::integer:
          try {
            return std::stoi(raw);
          } catch (...) {
            return raw;
          }
        case option_type_e::number:
          try {
            return std::stod(raw);
          } catch (...) {
            return raw;
          }
        case option_type_e::array:
        case option_type_e::object:
          try {
            return json::parse(raw);
          } catch (...) {
            return raw;
          }
        case option_type_e::string:
          return raw;
      }
      return raw;
    }

    json read_current_values(bool explicit_all) {
      json result = json::object();
      std::unordered_map<std::string, std::string> vars;
      try {
        vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      } catch (...) {
        vars = {};
      }

      for (const auto &definition : options()) {
        auto it = vars.find(definition.key);
        if (it != vars.end()) {
          result[definition.key] = parse_value(definition, it->second);
        } else if (explicit_all) {
          result[definition.key] = definition.default_value;
        }
      }
      return result;
    }

    std::string serialize(const json &input, bool explicit_all) {
      auto values = explicit_all ? effective_config_json() : json::object();
      for (const auto &[key, value] : input.items()) {
        values[key] = value;
      }

      std::stringstream stream;
      for (const auto &definition : options()) {
        if (!values.contains(definition.key)) {
          continue;
        }
        stream << definition.key << " = " << value_to_config_string(values[definition.key]) << '\n';
      }
      return stream.str();
    }
  }  // namespace

  const std::vector<option_definition_t> &options() {
    static const std::vector<option_definition_t> definitions {
      opt("locale", option_type_e::string, "en", "general", {}, {"bg", "cs", "de", "en", "en_GB", "en_US", "es", "fr", "hu", "it", "ja", "ko", "pl", "pt", "pt_BR", "ru", "sv", "tr", "uk", "vi", "zh", "zh_TW"}),
      opt("sunshine_name", option_type_e::string, "", "general"),
      opt("min_log_level", option_type_e::string, 2, "general", {}, {"verbose", "debug", "info", "warning", "error", "fatal", "none", "0", "1", "2", "3", "4", "5", "6"}),
      opt("global_prep_cmd", option_type_e::array, json::array(), "general"),
      opt("notify_pre_releases", option_type_e::boolean, "disabled", "general"),
      opt("system_tray", option_type_e::boolean, "enabled", "general"),

      opt("controller", option_type_e::boolean, "enabled", "input"),
      opt("gamepad", option_type_e::string, "auto", "input"),
      opt("ds4_back_as_touchpad_click", option_type_e::boolean, "enabled", "input"),
      opt("motion_as_ds4", option_type_e::boolean, "enabled", "input"),
      opt("touchpad_as_ds4", option_type_e::boolean, "enabled", "input"),
      opt("ds5_inputtino_randomize_mac", option_type_e::boolean, "enabled", "input"),
      opt("back_button_timeout", option_type_e::integer, -1, "input"),
      opt("keyboard", option_type_e::boolean, "enabled", "input"),
      opt("key_repeat_delay", option_type_e::integer, 500, "input"),
      opt("key_repeat_frequency", option_type_e::number, 24.9, "input"),
      opt("always_send_scancodes", option_type_e::boolean, "enabled", "input"),
      opt("key_rightalt_to_key_win", option_type_e::boolean, "disabled", "input"),
      opt("mouse", option_type_e::boolean, "enabled", "input"),
      opt("high_resolution_scrolling", option_type_e::boolean, "enabled", "input"),
      opt("native_pen_touch", option_type_e::boolean, "enabled", "input"),
      opt("keybindings", option_type_e::array, "[0x10,0xA0,0x11,0xA2,0x12,0xA4]", "input"),

      opt("audio_sink", option_type_e::string, "", "av"),
      opt("virtual_sink", option_type_e::string, "", "av"),
      opt("stream_audio", option_type_e::boolean, "enabled", "av"),
      opt("install_steam_audio_drivers", option_type_e::boolean, "enabled", "av"),
      opt("adapter_name", option_type_e::string, "", "av"),
      opt("output_name", option_type_e::string, "", "av"),
      opt("dd_configuration_option", option_type_e::string, "disabled", "av", {}, {"disabled", "verify_only", "ensure_active", "ensure_primary", "ensure_only_display"}),
      opt("dd_resolution_option", option_type_e::string, "auto", "av", {}, {"disabled", "auto", "manual"}),
      opt("dd_manual_resolution", option_type_e::string, "", "av"),
      opt("dd_refresh_rate_option", option_type_e::string, "auto", "av", {}, {"disabled", "auto", "manual"}),
      opt("dd_manual_refresh_rate", option_type_e::string, "", "av"),
      opt("dd_hdr_option", option_type_e::string, "auto", "av", {}, {"disabled", "auto"}),
      opt("dd_wa_hdr_toggle_delay", option_type_e::integer, 0, "av", {}, {}, 0, 3000),
      opt("dd_config_revert_delay", option_type_e::integer, 3000, "av", {}, {}, 0),
      opt("dd_config_revert_on_disconnect", option_type_e::boolean, "disabled", "av"),
      opt("dd_mode_remapping", option_type_e::object, json {{"mixed", json::array()}, {"resolution_only", json::array()}, {"refresh_rate_only", json::array()}}, "av"),
      opt("max_bitrate", option_type_e::integer, 0, "av"),
      opt("minimum_fps_target", option_type_e::number, 0, "av"),

      opt("upnp", option_type_e::boolean, "disabled", "network"),
      opt("address_family", option_type_e::string, "ipv4", "network", {}, {"ipv4", "both"}),
      opt("bind_address", option_type_e::string, "", "network"),
      opt("port", option_type_e::integer, 47989, "network", {}, {}, 1025, 65535),
      opt("origin_web_ui_allowed", option_type_e::string, "lan", "network", {}, {"pc", "lan", "wan"}),
      opt("csrf_allowed_origins", option_type_e::string, "", "network"),
      opt("external_ip", option_type_e::string, "", "network"),
      opt("lan_encryption_mode", option_type_e::integer, 0, "network", {}, {}, 0, 2),
      opt("wan_encryption_mode", option_type_e::integer, 1, "network", {}, {}, 0, 2),
      opt("ping_timeout", option_type_e::integer, 10000, "network"),

      opt("file_apps", option_type_e::string, "", "files"),
      opt("credentials_file", option_type_e::string, "", "files"),
      opt("log_path", option_type_e::string, "", "files"),
      opt("pkey", option_type_e::string, "", "files"),
      opt("cert", option_type_e::string, "", "files"),
      opt("file_state", option_type_e::string, "", "files"),

      opt("fec_percentage", option_type_e::integer, 20, "advanced", {}, {}, 1, 255),
      opt("qp", option_type_e::integer, 28, "advanced"),
      opt("min_threads", option_type_e::integer, 2, "advanced"),
      opt("hevc_mode", option_type_e::integer, 0, "advanced", {}, {}, 0, 3),
      opt("av1_mode", option_type_e::integer, 0, "advanced", {}, {}, 0, 3),
      opt("capture", option_type_e::string, "", "advanced"),
      opt("encoder", option_type_e::string, "", "advanced"),

      opt("app_streaming_enabled", option_type_e::boolean, "enabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_provider", option_type_e::string, "sudovda", "modules", "app_streaming", {"sudovda"}),
      opt("app_streaming_startup_cleanup", option_type_e::boolean, "enabled", "modules", "app_streaming"),
      opt("app_streaming_default_resolution", option_type_e::string, "client", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_default_client_display_mode", option_type_e::string, "windowed", "modules", "app_streaming", {"windowed", "borderless", "fullscreen"}, {}, {}, true, false),
      opt("app_streaming_default_client_app_window", option_type_e::boolean, "enabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_default_client_absolute_mouse", option_type_e::boolean, "enabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_default_show_cursor", option_type_e::boolean, "disabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_default_terminate_on_disconnect", option_type_e::boolean, "disabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_window_timeout_ms", option_type_e::integer, 60000, "modules", "app_streaming", {}, 0, {}, true, false),
      opt("app_streaming_window_follow_timeout_ms", option_type_e::integer, 1800000, "modules", "app_streaming", {}, 0, {}, true, false),
      opt("app_streaming_follow_windows", option_type_e::boolean, "enabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_borderless_windows", option_type_e::boolean, "enabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_discover_start_menu", option_type_e::boolean, "enabled", "modules", "app_streaming", {}, {}, {}, true, false),
      opt("app_streaming_sudovda_device_name", option_type_e::string, "SunshineVDD", "modules", "app_streaming"),
      opt("app_streaming_sudovda_serial", option_type_e::string, "Sunshine0001", "modules", "app_streaming"),

      opt("nvenc_preset", option_type_e::integer, 1, "nv", {}, {}, 1, 7),
      opt("nvenc_twopass", option_type_e::string, "quarter_res", "nv"),
      opt("nvenc_spatial_aq", option_type_e::boolean, "disabled", "nv"),
      opt("nvenc_vbv_increase", option_type_e::integer, 0, "nv", {}, {}, 0, 400),
      opt("nvenc_realtime_hags", option_type_e::boolean, "enabled", "nv"),
      opt("nvenc_split_encode", option_type_e::string, "driver_decides", "nv"),
      opt("nvenc_latency_over_power", option_type_e::boolean, "enabled", "nv"),
      opt("nvenc_opengl_vulkan_on_dxgi", option_type_e::boolean, "enabled", "nv"),
      opt("nvenc_h264_cavlc", option_type_e::boolean, "disabled", "nv"),

      opt("qsv_preset", option_type_e::string, "medium", "qsv"),
      opt("qsv_coder", option_type_e::string, "auto", "qsv"),
      opt("qsv_slow_hevc", option_type_e::boolean, "disabled", "qsv"),

      opt("amd_usage", option_type_e::string, "ultralowlatency", "amd"),
      opt("amd_rc", option_type_e::string, "vbr_latency", "amd"),
      opt("amd_enforce_hrd", option_type_e::boolean, "disabled", "amd"),
      opt("amd_quality", option_type_e::string, "balanced", "amd"),
      opt("amd_preanalysis", option_type_e::boolean, "disabled", "amd"),
      opt("amd_vbaq", option_type_e::boolean, "enabled", "amd"),
      opt("amd_coder", option_type_e::string, "auto", "amd"),

      opt("vt_coder", option_type_e::string, "auto", "vt"),
      opt("vt_software", option_type_e::string, "auto", "vt"),
      opt("vt_realtime", option_type_e::boolean, "enabled", "vt"),
      opt("vaapi_strict_rc_buffer", option_type_e::boolean, "disabled", "vaapi"),
      opt("vk_tune", option_type_e::integer, 2, "vulkan"),
      opt("vk_rc_mode", option_type_e::integer, 2, "vulkan"),
      opt("sw_preset", option_type_e::string, "superfast", "sw"),
      opt("sw_tune", option_type_e::string, "zerolatency", "sw"),
    };
    return definitions;
  }

  const option_definition_t *find_option(std::string_view key) {
    for (const auto &definition : options()) {
      if (definition.key == key) {
        return &definition;
      }
    }
    return nullptr;
  }

  bool is_known_key(std::string_view key) {
    return find_option(key) != nullptr;
  }

  json schema_json() {
    json output = json::array();
    for (const auto &definition : options()) {
      json node;
      node["key"] = definition.key;
      node["type"] = type_name(definition.type);
      node["default"] = definition.default_value;
      node["section"] = definition.section;
      node["module"] = definition.module;
      node["choices"] = definition.choices;
      node["runtime_mutable"] = definition.runtime_mutable;
      node["restart_required"] = definition.restart_required;
      if (definition.minimum) {
        node["minimum"] = *definition.minimum;
      }
      if (definition.maximum) {
        node["maximum"] = *definition.maximum;
      }
      output.push_back(std::move(node));
    }
    return output;
  }

  json schema_json_for_module(std::string_view module_id) {
    json output = json::array();
    for (const auto &definition : options()) {
      if (definition.module == module_id) {
        json node;
        node["key"] = definition.key;
        node["type"] = type_name(definition.type);
        node["default"] = definition.default_value;
        node["choices"] = definition.choices;
        node["runtime_mutable"] = definition.runtime_mutable;
        node["restart_required"] = definition.restart_required;
        if (definition.minimum) {
          node["minimum"] = *definition.minimum;
        }
        if (definition.maximum) {
          node["maximum"] = *definition.maximum;
        }
        output.push_back(std::move(node));
      }
    }
    return output;
  }

  json effective_config_json() {
    return read_current_values(true);
  }

  json effective_config_json_for_module(std::string_view module_id) {
    auto all_values = effective_config_json();
    json output = json::object();
    for (const auto &definition : options()) {
      if (definition.module == module_id && all_values.contains(definition.key)) {
        output[definition.key] = all_values[definition.key];
      }
    }
    return output;
  }

  bool validate_config(const json &input, std::vector<std::string> &errors) {
    if (!input.is_object()) {
      errors.emplace_back("Config payload must be a JSON object.");
      return false;
    }

    for (const auto &[key, value] : input.items()) {
      const auto *definition = find_option(key);
      if (!definition) {
        errors.emplace_back(std::format("Unknown configurable option [{}].", key));
        continue;
      }

      std::string error;
      if (!validate_type(*definition, value, error)) {
        errors.emplace_back(std::format("Invalid value for [{}]: {}", key, error));
      }
    }

    return errors.empty();
  }

  std::string value_to_config_string(const json &value) {
    if (value.is_boolean()) {
      return value.get<bool>() ? "enabled" : "disabled";
    }
    if (value.is_string()) {
      return value.get<std::string>();
    }
    return value.dump();
  }

  bool write_config(const json &input, bool explicit_all, std::string &error) {
    std::vector<std::string> errors;
    if (!validate_config(input, errors)) {
      error = join_strings(errors, "\n");
      return false;
    }

    try {
      file_handler::write_file(config::sunshine.config_file.c_str(), serialize(input, explicit_all));
      return true;
    } catch (const std::exception &err) {
      error = err.what();
      return false;
    }
  }

  bool patch_config(const json &patch, bool explicit_all, std::string &error) {
    if (!patch.is_object()) {
      error = "Config patch payload must be a JSON object.";
      return false;
    }

    for (const auto &[key, value] : patch.items()) {
      const auto *definition = find_option(key);
      if (!definition) {
        error = std::format("Unknown configurable option [{}].", key);
        return false;
      }

      if (!value.is_null()) {
        std::string value_error;
        if (!validate_type(*definition, value, value_error)) {
          error = std::format("Invalid value for [{}]: {}", key, value_error);
          return false;
        }
      }
    }

    auto merged = explicit_all ? effective_config_json() : read_current_values(false);
    for (const auto &[key, value] : patch.items()) {
      if (value.is_null()) {
        if (explicit_all) {
          merged[key] = find_option(key)->default_value;
        } else {
          merged.erase(key);
        }
      } else {
        merged[key] = value;
      }
    }

    return write_config(merged, explicit_all, error);
  }
}  // namespace config_schema
