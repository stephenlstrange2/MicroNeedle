#include "command_validator.h"

#include "task_executor.h"

namespace {
bool fail(const char *code, const char **error) {
  if (error) *error = code;
  return false;
}

bool validAlias(const char *input) {
  char normalized[MICRONEEDLE_ALIAS_SIZE];
  return DeviceRegistry::normalizeAlias(input, normalized, sizeof(normalized));
}
}  // namespace

bool CommandValidator::validate(const TaskRequest &request, const DeviceRegistry &registry,
                                const char **error) {
  if (!validAlias(request.alias)) return fail("invalid_alias", error);
  const DeviceBinding *binding = registry.find(request.alias);

  switch (request.operation) {
    case TaskOperation::DEVICE_BIND: {
      if (binding) return fail("alias_exists", error);
      const char *reason = nullptr;
      if (!BoardProfile::canBindOutput(request.pin, &reason))
        return fail(reason ? reason : "reserved_pin", error);
      if (registry.pinClaimed(request.pin)) return fail("pin_already_claimed", error);
      return true;
    }
    case TaskOperation::DEVICE_UNBIND:
    case TaskOperation::DEVICE_RENAME:
      if (!binding) return fail("device_not_found", error);
      if (binding->driver == DriverType::WS2812_RGB)
        return fail("system_device_immutable", error);
      if (request.operation == TaskOperation::DEVICE_RENAME) {
        if (!validAlias(request.newAlias)) return fail("invalid_alias", error);
        const DeviceBinding *existing = registry.find(request.newAlias);
        if (existing && existing != binding) return fail("alias_exists", error);
      }
      return true;
    case TaskOperation::DEVICE_SET:
      if (!binding) return fail("device_not_found", error);
      if (!(binding->capabilities & (request.state ? CAP_ON : CAP_OFF)))
        return fail("operation_not_supported", error);
      return true;
    case TaskOperation::DEVICE_SET_RGB:
      if (!binding) return fail("device_not_found", error);
      if (!(binding->capabilities & CAP_COLOR) || !(binding->capabilities & CAP_BRIGHTNESS))
        return fail("operation_not_supported", error);
      return true;
    case TaskOperation::DEVICE_SET_PATTERN:
      if (!binding) return fail("device_not_found", error);
      if (!(binding->capabilities & CAP_PATTERN)) return fail("operation_not_supported", error);
      if (static_cast<uint8_t>(request.pattern) > static_cast<uint8_t>(LedPattern::RAINBOW))
        return fail("invalid_pattern", error);
      return true;
    case TaskOperation::DEVICE_GET:
      if (!binding) return fail("device_not_found", error);
      if (!(binding->capabilities & CAP_STATE)) return fail("operation_not_supported", error);
      return true;
  }
  return fail("unknown_operation", error);
}
