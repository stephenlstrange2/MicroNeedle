#pragma once

struct TaskRequest;
class DeviceRegistry;

class CommandValidator {
 public:
  static bool validate(const TaskRequest &request, const DeviceRegistry &registry,
                       const char **error = nullptr);
};
