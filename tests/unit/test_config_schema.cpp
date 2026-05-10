/**
 * @file tests/unit/test_config_schema.cpp
 * @brief Test config schema validation and module option registration.
 */
#include "../tests_common.h"

#include <set>

#include <src/config_schema.h>
#include <src/module_registry.h>

TEST(ConfigSchemaTests, RejectsUnknownConfigKeys) {
  nlohmann::json payload {
    {"not_a_real_sunshine_option", "value"}
  };
  std::vector<std::string> errors;

  ASSERT_FALSE(config_schema::validate_config(payload, errors));
  ASSERT_FALSE(errors.empty());
}

TEST(ConfigSchemaTests, AcceptsAppStreamingModuleSettings) {
  nlohmann::json payload {
    {"app_streaming_enabled", "enabled"},
    {"app_streaming_provider", "sudovda"},
    {"app_streaming_default_resolution", "client"}
  };
  std::vector<std::string> errors;

  ASSERT_TRUE(config_schema::validate_config(payload, errors)) << errors.front();
}

TEST(ConfigSchemaTests, RejectsInvalidNumericSettings) {
  nlohmann::json payload {
    {"app_streaming_window_timeout_ms", "not-a-number"}
  };
  std::vector<std::string> errors;

  ASSERT_FALSE(config_schema::validate_config(payload, errors));
  ASSERT_FALSE(errors.empty());
}

TEST(ConfigSchemaTests, AppStreamingSettingsAreRegisteredForModule) {
  const auto schema = config_schema::schema_json_for_module("app_streaming");
  std::set<std::string> keys;

  for (const auto &entry : schema) {
    keys.insert(entry["key"].get<std::string>());
  }

  ASSERT_TRUE(keys.contains("app_streaming_enabled"));
  ASSERT_TRUE(keys.contains("app_streaming_provider"));
  ASSERT_TRUE(keys.contains("app_streaming_window_timeout_ms"));
  ASSERT_TRUE(keys.contains("app_streaming_sudovda_device_name"));
  ASSERT_TRUE(keys.contains("app_streaming_sudovda_serial"));
}

TEST(ModuleRegistryTests, ListsAppStreamingModule) {
  const auto modules = modules::list_json();
  ASSERT_FALSE(modules.empty());
  ASSERT_EQ(modules.front()["id"], "app_streaming");
}
