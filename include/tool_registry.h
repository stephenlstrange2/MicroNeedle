#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

static constexpr size_t MICRONEEDLE_MAX_TOOL_ARGS = 6;

enum class ToolId : uint8_t {
  DEVICE_INFO,
  DEVICE_CAPABILITIES,
  LED_SET,
  LED_OFF,
  LED_PATTERN,
  DEVICE_BIND,
  DEVICE_UNBIND,
  DEVICE_RENAME,
  DEVICE_LIST,
  DEVICE_DESCRIBE,
  DEVICE_GET,
  DEVICE_SET,
  TASK_STATUS,
  TASK_CANCEL,
  TASK_LIST,
  CONFIRMATION_CONFIRM,
};

enum class ToolArgType : uint8_t {
  STRING,
  INTEGER,
  UNSIGNED_INTEGER,
  BOOLEAN_OR_STATE,
};

enum ToolFlags : uint8_t {
  TOOL_NONE = 0,
  TOOL_MUTATES_HARDWARE = 1 << 0,
  TOOL_CONFIGURATION = 1 << 1,
  TOOL_REQUIRES_CONFIRMATION = 1 << 2,
};

struct ToolArgRule {
  const char *name = nullptr;
  ToolArgType type = ToolArgType::STRING;
  bool required = false;
  int32_t minimum = INT32_MIN;
  int32_t maximum = INT32_MAX;
  const char *const *allowedValues = nullptr;
  uint8_t allowedValueCount = 0;
};

struct ToolDefinition {
  ToolId id;
  const char *name;
  uint8_t flags;
  ToolArgRule args[MICRONEEDLE_MAX_TOOL_ARGS];
  uint8_t argCount;
};

class ToolRegistry {
 public:
  static const ToolDefinition *find(const char *name);
  static const ToolDefinition *at(size_t index);
  static size_t count();
  static const char *argTypeName(ToolArgType type);

  // Validates required fields, types, enums, ranges, and unknown arguments.
  static bool validate(const ToolDefinition &definition, JsonObjectConst args,
                       char *message, size_t messageSize);
};
