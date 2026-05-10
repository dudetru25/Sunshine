/**
 * @file src/config_schema.h
 * @brief Registry and validation helpers for Sunshine configuration options.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace config_schema {
  enum class option_type_e {
    string,
    integer,
    number,
    boolean,
    array,
    object
  };

  struct option_definition_t {
    std::string key;
    option_type_e type;
    nlohmann::json default_value;
    std::string section;
    std::string module;
    std::vector<std::string> choices;
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool runtime_mutable {};
    bool restart_required {true};
  };

  const std::vector<option_definition_t> &options();
  const option_definition_t *find_option(std::string_view key);
  bool is_known_key(std::string_view key);

  nlohmann::json schema_json();
  nlohmann::json schema_json_for_module(std::string_view module_id);
  nlohmann::json effective_config_json();
  nlohmann::json effective_config_json_for_module(std::string_view module_id);

  bool validate_config(const nlohmann::json &input, std::vector<std::string> &errors);
  bool write_config(const nlohmann::json &input, bool explicit_all, std::string &error);
  bool patch_config(const nlohmann::json &patch, bool explicit_all, std::string &error);
  std::string value_to_config_string(const nlohmann::json &value);
}  // namespace config_schema
