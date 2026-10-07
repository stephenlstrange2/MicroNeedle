#include "tool_registry.h"

#include <cstdio>
#include <cstring>

namespace {
const char *const ACTIVE_LEVELS[] = {"high", "low"};
const char *const STATES[] = {"on", "off"};
const char *const PATTERNS[] = {"solid", "blink", "pulse", "rainbow"};

constexpr ToolArgRule stringArg(const char *name, bool required,
                                const char *const *allowed = nullptr, uint8_t allowedCount = 0) {
  return {name, ToolArgType::STRING, required, INT32_MIN, INT32_MAX, allowed, allowedCount};
}

constexpr ToolArgRule intArg(const char *name, bool required, int32_t minimum, int32_t maximum) {
  return {name, ToolArgType::INTEGER, required, minimum, maximum, nullptr, 0};
}

constexpr ToolArgRule uintArg(const char *name, bool required) {
  return {name, ToolArgType::UNSIGNED_INTEGER, required, 0, INT32_MAX, nullptr, 0};
}

constexpr ToolArgRule stateArg(const char *name, bool required) {
  return {name, ToolArgType::BOOLEAN_OR_STATE, required, 0, 0, STATES, 2};
}

const ToolDefinition DEFINITIONS[] = {
    {ToolId::DEVICE_INFO, "device.info", TOOL_NONE, {}, 0},
    {ToolId::DEVICE_CAPABILITIES, "device.capabilities", TOOL_NONE, {}, 0},
    {ToolId::LED_SET, "led.set", TOOL_MUTATES_HARDWARE,
     {intArg("r", true, 0, 255), intArg("g", true, 0, 255), intArg("b", true, 0, 255),
      intArg("brightness", false, 0, 255)}, 4},
    {ToolId::LED_OFF, "led.off", TOOL_MUTATES_HARDWARE, {}, 0},
    {ToolId::LED_PATTERN, "led.pattern", TOOL_MUTATES_HARDWARE,
     {stringArg("pattern", true, PATTERNS, 4)}, 1},
    {ToolId::DEVICE_BIND, "device.bind",
     TOOL_CONFIGURATION | TOOL_REQUIRES_CONFIRMATION,
     {stringArg("alias", true), intArg("pin", true, 0, 48),
      stringArg("active_level", false, ACTIVE_LEVELS, 2),
      stringArg("startup_state", false, STATES, 2)}, 4},
    {ToolId::DEVICE_UNBIND, "device.unbind",
     TOOL_CONFIGURATION | TOOL_REQUIRES_CONFIRMATION,
     {stringArg("alias", true)}, 1},
    {ToolId::DEVICE_RENAME, "device.rename",
     TOOL_CONFIGURATION | TOOL_REQUIRES_CONFIRMATION,
     {stringArg("alias", true), stringArg("new_alias", true)}, 2},
    {ToolId::DEVICE_LIST, "device.list", TOOL_NONE, {}, 0},
    {ToolId::DEVICE_DESCRIBE, "device.describe", TOOL_NONE,
     {stringArg("alias", true)}, 1},
    {ToolId::DEVICE_GET, "device.get", TOOL_NONE, {stringArg("alias", true)}, 1},
    {ToolId::DEVICE_SET, "device.set", TOOL_MUTATES_HARDWARE,
     {stringArg("alias", true), stateArg("state", true)}, 2},
    {ToolId::TASK_STATUS, "task.status", TOOL_NONE, {uintArg("task_id", true)}, 1},
    {ToolId::TASK_CANCEL, "task.cancel", TOOL_NONE, {uintArg("task_id", true)}, 1},
    {ToolId::TASK_LIST, "task.list", TOOL_NONE, {}, 0},
    {ToolId::CONFIRMATION_CONFIRM, "confirmation.confirm", TOOL_CONFIGURATION,
     {uintArg("confirmation_id", true)}, 1},
};

const ToolArgRule *findRule(const ToolDefinition &definition, const char *name) {
  for (uint8_t i = 0; i < definition.argCount; ++i)
    if (strcmp(definition.args[i].name, name) == 0) return &definition.args[i];
  return nullptr;
}

bool allowedString(const ToolArgRule &rule, const char *value) {
  if (!rule.allowedValues) return value && value[0];
  for (uint8_t i = 0; i < rule.allowedValueCount; ++i)
    if (strcmp(value, rule.allowedValues[i]) == 0) return true;
  return false;
}

void setMessage(char *message, size_t size, const char *prefix, const char *name) {
  if (!message || !size) return;
  snprintf(message, size, "%s%s", prefix, name ? name : "");
}
}  // namespace

const ToolDefinition *ToolRegistry::find(const char *name) {
  if (!name) return nullptr;
  for (const auto &definition : DEFINITIONS)
    if (strcmp(definition.name, name) == 0) return &definition;
  return nullptr;
}

const ToolDefinition *ToolRegistry::at(size_t index) {
  return index < count() ? &DEFINITIONS[index] : nullptr;
}

size_t ToolRegistry::count() {
  return sizeof(DEFINITIONS) / sizeof(DEFINITIONS[0]);
}

const char *ToolRegistry::argTypeName(ToolArgType type) {
  switch (type) {
    case ToolArgType::STRING: return "string";
    case ToolArgType::INTEGER: return "integer";
    case ToolArgType::UNSIGNED_INTEGER: return "unsigned_integer";
    case ToolArgType::BOOLEAN_OR_STATE: return "boolean_or_state";
  }
  return "unknown";
}

bool ToolRegistry::validate(const ToolDefinition &definition, JsonObjectConst args,
                            char *message, size_t messageSize) {
  for (uint8_t i = 0; i < definition.argCount; ++i) {
    const ToolArgRule &rule = definition.args[i];
    JsonVariantConst value = args[rule.name];
    if (value.isNull()) {
      if (rule.required) {
        setMessage(message, messageSize, "Missing required argument: ", rule.name);
        return false;
      }
      continue;
    }

    bool valid = false;
    switch (rule.type) {
      case ToolArgType::STRING:
        valid = value.is<const char *>() && allowedString(rule, value.as<const char *>());
        break;
      case ToolArgType::INTEGER:
        if (value.is<int>()) {
          const int32_t number = value.as<int32_t>();
          valid = number >= rule.minimum && number <= rule.maximum;
        }
        break;
      case ToolArgType::UNSIGNED_INTEGER:
        valid = value.is<uint32_t>();
        break;
      case ToolArgType::BOOLEAN_OR_STATE:
        valid = value.is<bool>() ||
                (value.is<const char *>() && allowedString(rule, value.as<const char *>()));
        break;
    }
    if (!valid) {
      setMessage(message, messageSize, "Invalid argument: ", rule.name);
      return false;
    }
  }

  for (JsonPairConst pair : args) {
    if (!findRule(definition, pair.key().c_str())) {
      setMessage(message, messageSize, "Unknown argument: ", pair.key().c_str());
      return false;
    }
  }
  return true;
}
