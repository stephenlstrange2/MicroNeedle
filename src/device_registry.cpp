#include "device_registry.h"

#include <Preferences.h>
#include <ctype.h>
#include <cstddef>
#include <cstring>

namespace {
constexpr uint32_t STORE_MAGIC = 0x4D4E4456;  // MNDV
constexpr uint16_t STORE_VERSION = 1;
constexpr char NVS_NAMESPACE[] = "mn_devices";
constexpr char NVS_KEY[] = "registry";

struct RegistryStore {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t nextId;
  DeviceBinding bindings[MICRONEEDLE_MAX_DEVICES];
  uint32_t checksum;
};

uint32_t checksumBytes(const uint8_t *data, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}
}  // namespace

bool DeviceRegistry::normalizeAlias(const char *input, char *output, size_t outputSize) {
  if (!input || !output || outputSize < 2) return false;
  size_t written = 0;
  bool separator = false;
  while (*input && isspace(static_cast<unsigned char>(*input))) ++input;
  for (; *input && written + 1 < outputSize; ++input) {
    const unsigned char c = static_cast<unsigned char>(*input);
    if (isalnum(c)) {
      if (separator && written > 0 && written + 1 < outputSize) output[written++] = '_';
      output[written++] = static_cast<char>(tolower(c));
      separator = false;
    } else if (c == '_' || c == '-' || isspace(c)) {
      separator = written > 0;
    } else {
      return false;
    }
  }
  if (*input) {
    output[0] = '\0';
    return false;
  }
  output[written] = '\0';
  return written > 0;
}

bool DeviceRegistry::begin() {
  if (!load()) {
    memset(bindings_, 0, sizeof(bindings_));
    nextId_ = 1;
  }
  bool sanitized = false;
  for (size_t i = 0; i < MICRONEEDLE_MAX_DEVICES; ++i) {
    DeviceBinding &binding = bindings_[i];
    if (!binding.occupied) continue;
    binding.alias[MICRONEEDLE_ALIAS_SIZE - 1] = '\0';
    binding.displayName[MICRONEEDLE_ALIAS_SIZE - 1] = '\0';
    const char *reason = nullptr;
    DeviceDriver *driver = driverFor(binding.driver);
    if (!binding.alias[0] || !BoardProfile::canBindOutput(binding.pin, &reason) ||
        !driver || !driver->begin(binding)) {
      binding.occupied = false;
      sanitized = true;
    }
  }
  if (sanitized) save();
  return true;
}

bool DeviceRegistry::load() {
  Preferences preferences;
  // Opening read/write creates the namespace on first boot. Read-only open
  // reports NOT_FOUND when no binding has ever been saved, which is normal.
  if (!preferences.begin(NVS_NAMESPACE, false)) return false;
  const size_t length = preferences.getBytesLength(NVS_KEY);
  if (length != sizeof(RegistryStore)) {
    preferences.end();
    return false;
  }
  RegistryStore store{};
  const size_t read = preferences.getBytes(NVS_KEY, &store, sizeof(store));
  preferences.end();
  if (read != sizeof(store) || store.magic != STORE_MAGIC || store.version != STORE_VERSION) return false;
  const uint32_t expected = checksumBytes(reinterpret_cast<const uint8_t *>(&store),
                                          offsetof(RegistryStore, checksum));
  if (expected != store.checksum || store.count > MICRONEEDLE_MAX_DEVICES) return false;
  memcpy(bindings_, store.bindings, sizeof(bindings_));
  nextId_ = store.nextId ? store.nextId : 1;
  return true;
}

bool DeviceRegistry::save() {
  RegistryStore store{};
  store.magic = STORE_MAGIC;
  store.version = STORE_VERSION;
  store.count = static_cast<uint16_t>(count());
  store.nextId = nextId_;
  memcpy(store.bindings, bindings_, sizeof(bindings_));
  store.checksum = checksumBytes(reinterpret_cast<const uint8_t *>(&store),
                                 offsetof(RegistryStore, checksum));
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false)) return false;
  const size_t written = preferences.putBytes(NVS_KEY, &store, sizeof(store));
  preferences.end();
  return written == sizeof(store);
}

int DeviceRegistry::freeSlot() const {
  for (size_t i = 0; i < MICRONEEDLE_MAX_DEVICES; ++i) {
    if (!bindings_[i].occupied) return static_cast<int>(i);
  }
  return -1;
}

DeviceOperationResult DeviceRegistry::bindGpioOutput(const char *displayName, uint8_t pin,
                                                      bool activeHigh, bool startupOn,
                                                      DeviceBinding **created) {
  char alias[MICRONEEDLE_ALIAS_SIZE];
  if (!normalizeAlias(displayName, alias, sizeof(alias))) return {false, "invalid_alias", false};
  if (find(alias)) return {false, "alias_exists", false};
  const char *reason = nullptr;
  if (!BoardProfile::canBindOutput(pin, &reason)) return {false, reason ? reason : "reserved_pin", false};
  if (pinClaimed(pin)) return {false, "pin_already_claimed", false};
  const int slot = freeSlot();
  if (slot < 0) return {false, "registry_full", false};

  DeviceBinding &binding = bindings_[slot];
  const uint32_t previousNextId = nextId_;
  memset(&binding, 0, sizeof(binding));
  binding.id = nextId_++;
  strlcpy(binding.alias, alias, sizeof(binding.alias));
  strlcpy(binding.displayName, displayName, sizeof(binding.displayName));
  binding.driver = DriverType::GPIO_OUTPUT;
  binding.pin = pin;
  binding.activeHigh = activeHigh;
  binding.startupOn = startupOn;
  binding.capabilities = CAP_ON | CAP_OFF | CAP_STATE;
  binding.occupied = true;

  DeviceDriver *driver = driverFor(binding.driver);
  if (!driver || !driver->begin(binding)) {
    memset(&binding, 0, sizeof(binding));
    nextId_ = previousNextId;
    return {false, "driver_init_failed", false};
  }
  if (!save()) {
    driver->safeStop(binding);
    memset(&binding, 0, sizeof(binding));
    nextId_ = previousNextId;
    return {false, "persistence_failed", false};
  }
  if (created) *created = &binding;
  return {true, nullptr, binding.currentOn};
}

DeviceOperationResult DeviceRegistry::unbind(const char *alias) {
  DeviceBinding *binding = find(alias);
  if (!binding) return {false, "device_not_found", false};
  DeviceBinding previous = *binding;
  DeviceDriver *driver = driverFor(binding->driver);
  if (driver) driver->safeStop(*binding);
  memset(binding, 0, sizeof(*binding));
  if (!save()) {
    *binding = previous;
    if (driver) {
      driver->begin(*binding);
      driver->set(*binding, previous.currentOn);
    }
    return {false, "persistence_failed", previous.currentOn};
  }
  return {true, nullptr, false};
}

DeviceOperationResult DeviceRegistry::rename(const char *alias, const char *newDisplayName) {
  DeviceBinding *binding = find(alias);
  if (!binding) return {false, "device_not_found", false};
  char normalized[MICRONEEDLE_ALIAS_SIZE];
  if (!normalizeAlias(newDisplayName, normalized, sizeof(normalized))) return {false, "invalid_alias", false};
  DeviceBinding *existing = find(normalized);
  if (existing && existing != binding) return {false, "alias_exists", false};
  DeviceBinding previous = *binding;
  strlcpy(binding->alias, normalized, sizeof(binding->alias));
  strlcpy(binding->displayName, newDisplayName, sizeof(binding->displayName));
  if (!save()) {
    *binding = previous;
    return {false, "persistence_failed", previous.currentOn};
  }
  return {true, nullptr, binding->currentOn};
}

DeviceOperationResult DeviceRegistry::set(const char *alias, bool on) {
  DeviceBinding *binding = find(alias);
  if (!binding) return {false, "device_not_found", false};
  if (!(binding->capabilities & (on ? CAP_ON : CAP_OFF))) return {false, "operation_not_supported", false};
  DeviceDriver *driver = driverFor(binding->driver);
  if (!driver) return {false, "driver_unavailable", false};
  return driver->set(*binding, on);
}

DeviceOperationResult DeviceRegistry::get(const char *alias) {
  DeviceBinding *binding = find(alias);
  if (!binding) return {false, "device_not_found", false};
  if (!(binding->capabilities & CAP_STATE)) return {false, "operation_not_supported", false};
  DeviceDriver *driver = driverFor(binding->driver);
  if (!driver) return {false, "driver_unavailable", false};
  return driver->get(*binding);
}

DeviceBinding *DeviceRegistry::find(const char *alias) {
  char normalized[MICRONEEDLE_ALIAS_SIZE];
  if (!normalizeAlias(alias, normalized, sizeof(normalized))) return nullptr;
  for (auto &binding : bindings_) {
    if (binding.occupied && strcmp(binding.alias, normalized) == 0) return &binding;
  }
  return nullptr;
}

const DeviceBinding *DeviceRegistry::find(const char *alias) const {
  return const_cast<DeviceRegistry *>(this)->find(alias);
}

DeviceBinding *DeviceRegistry::bindingAt(size_t slot) {
  return slot < MICRONEEDLE_MAX_DEVICES ? &bindings_[slot] : nullptr;
}

const DeviceBinding *DeviceRegistry::bindingAt(size_t slot) const {
  return slot < MICRONEEDLE_MAX_DEVICES ? &bindings_[slot] : nullptr;
}

size_t DeviceRegistry::count() const {
  size_t total = 0;
  for (const auto &binding : bindings_) if (binding.occupied) ++total;
  return total;
}

bool DeviceRegistry::pinClaimed(uint8_t pin) const {
  for (const auto &binding : bindings_) {
    if (binding.occupied && binding.pin == pin) return true;
  }
  return false;
}

void DeviceRegistry::clear() {
  for (auto &binding : bindings_) {
    if (!binding.occupied) continue;
    DeviceDriver *driver = driverFor(binding.driver);
    if (driver) driver->safeStop(binding);
  }
  memset(bindings_, 0, sizeof(bindings_));
  nextId_ = 1;
  Preferences preferences;
  if (preferences.begin(NVS_NAMESPACE, false)) {
    preferences.remove(NVS_KEY);
    preferences.end();
  }
}
