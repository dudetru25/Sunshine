/**
 * @file src/module_registry.h
 * @brief Built-in Sunshine module registry and action dispatch.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace modules {
  struct action_result_t {
    bool status {};
    std::string error;
    nlohmann::json data = nlohmann::json::object();
  };

  void init();
  bool exists(std::string_view module_id);

  nlohmann::json list_json();
  nlohmann::json module_json(std::string_view module_id);
  nlohmann::json settings_json(std::string_view module_id);
  nlohmann::json actions_json(std::string_view module_id);

  action_result_t invoke_action(std::string_view module_id, std::string_view action, const nlohmann::json &payload = nlohmann::json::object(), bool force = false);
  action_result_t update_settings(std::string_view module_id, const nlohmann::json &settings);
}  // namespace modules
