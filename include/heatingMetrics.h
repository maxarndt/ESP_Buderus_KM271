#pragma once

#include <stddef.h>
#include <stdint.h>

namespace metrics {

// Raw values for just the exported registers; receipt is independent of value.
// Updated and copied only in the main loop, never from the HTTP task.
struct HeatingSnapshot {
  enum Field : uint8_t {
    OUTSIDE, OUTSIDE_DAMPED, BOILER_TARGET, BOILER_ACTUAL,
    HC1_TARGET, HC1_ACTUAL, HC1_PUMP, HC1_FAULTS,
    HC2_TARGET, HC2_ACTUAL, HC2_PUMP, HC2_FAULTS,
    HOT_WATER_TARGET, HOT_WATER_ACTUAL, HOT_WATER_PUMPS, HOT_WATER_FAULTS,
    BOILER_FAULTS, BURNER, RUNTIME_HIGH, RUNTIME_MIDDLE, RUNTIME_LOW, FIELD_COUNT
  };

  static_assert(FIELD_COUNT <= 32, "Receipt mask must fit in 32 bits");

  uint8_t values[FIELD_COUNT] = {};
  uint32_t received = 0;
  uint32_t burnerRuntimeMinutes = 0;
  bool burnerRuntimeValid = false;
  bool circuitEnabled[2] = {};
  bool hotWaterEnabled = false;

  void observe(uint16_t address, uint8_t value);
  bool has(Field field) const { return (received & (UINT32_C(1) << field)) != 0; }
};

// Empty output is valid when no selected registers have been received yet.
size_t formatHeatingMetrics(char *buffer, size_t capacity, const HeatingSnapshot &snapshot,
                            const char (&device)[128], bool includeDeviceLabel);
size_t heatingMetricsSize(const HeatingSnapshot &snapshot, const char (&device)[128], bool includeDeviceLabel);

} // namespace metrics
