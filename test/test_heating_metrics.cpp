#include <hardwareMetrics.h>
#include <heatingMetrics.h>
#include "metrics_test_helpers.h"

using metrics::HeatingSnapshot;

static char device[128] = {};

static std::string render(const HeatingSnapshot &snapshot, bool withDevice = false) {
  const size_t length = metrics::heatingMetricsSize(snapshot, device, withDevice);
  std::vector<char> buffer(length + 1, '!');
  assert(metrics::formatHeatingMetrics(buffer.data(), buffer.size(), snapshot, device, withDevice) == length);
  assert(buffer[length] == '\0');
  return buffer.data();
}

static void expect(const std::string &text, const char *line) { assert(text.find(line) != std::string::npos); }
static void absent(const std::string &text, const char *name) { assert(text.find(name) == std::string::npos); }

static HeatingSnapshot fullSnapshot() {
  HeatingSnapshot snapshot;
  snapshot.circuitEnabled[0] = snapshot.circuitEnabled[1] = snapshot.hotWaterEnabled = true;
  const uint16_t addresses[] = {0x893c, 0x893d, 0x882a, 0x882b, 0x8002, 0x8003, 0x8008, 0x8001,
                                0x8114, 0x8115, 0x811a, 0x8113, 0x8426, 0x8427, 0x8429, 0x8424,
                                0x8830, 0x8832, 0x8836, 0x8837, 0x8838};
  const uint8_t values[] = {0xfb, 3, 70, 65, 45, 42, 75, 0, 35, 32, 50, 0, 55, 52, 1, 0, 0, 1, 1, 2, 3};
  static_assert(sizeof(addresses) / sizeof(addresses[0]) == sizeof(values), "Fixture mismatch");
  for (size_t i = 0; i < sizeof(values); ++i) {
    snapshot.observe(addresses[i], values[i]);
  }
  return snapshot;
}

int main() {
  HeatingSnapshot empty;
  empty.circuitEnabled[0] = empty.circuitEnabled[1] = empty.hotWaterEnabled = true;
  assert(render(empty).empty()); // Initial zeros must not become measurements.
  empty.observe(0x0400, 123); // Unknown/unexported registers have no effect.
  assert(render(empty).empty());
  assert(metrics::formatHeatingMetrics(nullptr, 0, empty, device, false) == 0);

  HeatingSnapshot zero;
  zero.observe(0x893c, 0);
  const std::string zeroText = render(zero);
  checkExposition(zeroText.c_str(), 1, false);
  expect(zeroText, "km271_outside_temperature_celsius 0\n");
  absent(zeroText, "outside_damped");

  HeatingSnapshot snapshot = fullSnapshot();
  std::string text = render(snapshot);
  checkExposition(text.c_str(), 36, false);
  expect(text, "km271_outside_temperature_celsius -5\n");
  expect(text, "km271_outside_damped_temperature_celsius 3\n");
  expect(text, "km271_boiler_temperature_celsius 65\n");
  expect(text, "km271_boiler_target_temperature_celsius 70\n");
  expect(text, "km271_heating_circuit_flow_temperature_celsius{circuit=\"1\"} 42\n");
  expect(text, "km271_heating_circuit_flow_target_temperature_celsius{circuit=\"2\"} 35\n");
  expect(text, "km271_heating_circuit_pump_ratio{circuit=\"1\"} 0.75\n");
  expect(text, "km271_heating_circuit_pump_ratio{circuit=\"2\"} 0.50\n");
  expect(text, "km271_hot_water_temperature_celsius 52\n");
  expect(text, "km271_hot_water_target_temperature_celsius 55\n");
  expect(text, "km271_hot_water_pump_active{pump=\"charging\"} 1\n");
  expect(text, "km271_hot_water_pump_active{pump=\"circulation\"} 0\n");
  expect(text, "km271_burner_active 1\n");
  expect(text, "# TYPE km271_burner_runtime_seconds_total counter\n");
  expect(text, "km271_burner_runtime_seconds_total 3963060\n"); // 0x010203 minutes.
  absent(text, "logging_active");
  absent(text, "telegram_age");
  absent(text, "room_temperature");
  absent(text, "solar");
  absent(text, "km271_oil");

  snapshot.circuitEnabled[0] = snapshot.circuitEnabled[1] = snapshot.hotWaterEnabled = false;
  text = render(snapshot);
  checkExposition(text.c_str(), 14, false);
  absent(text, "heating_circuit");
  absent(text, "hot_water");
  snapshot.circuitEnabled[1] = true;
  text = render(snapshot);
  checkExposition(text.c_str(), 21, false);
  absent(text, "circuit=\"1\"");
  absent(text, "heating_circuit_1");

  // A received sensor fault hides only the corresponding actual temperature.
  snapshot = fullSnapshot();
  snapshot.observe(0x8830, 1 << 1);
  snapshot.observe(0x8001, 1 << 4);
  snapshot.observe(0x8424, 1 << 5);
  text = render(snapshot);
  checkExposition(text.c_str(), 33, false);
  absent(text, "km271_boiler_temperature_celsius");
  absent(text, "km271_hot_water_temperature_celsius");
  absent(text, "km271_heating_circuit_flow_temperature_celsius{circuit=\"1\"}");
  expect(text, "km271_fault_active{component=\"boiler\",fault=\"boiler_sensor\"} 1\n");
  expect(text, "km271_fault_active{component=\"heating_circuit_1\",fault=\"flow_sensor\"} 1\n");
  expect(text, "km271_fault_active{component=\"hot_water\",fault=\"temperature_sensor\"} 1\n");
  snapshot.observe(0x8830, 0);
  snapshot.observe(0x8001, 0);
  snapshot.observe(0x8424, 0);
  checkExposition(render(snapshot).c_str(), 36, false);

  // Check each fault bit against its documented meaning, including the
  // external fault input (which is not an external temperature sensor).
  const char *boilerFaults[] = {"burner", "boiler_sensor", "auxiliary_sensor", "stays_cold", "exhaust_sensor", "exhaust_limit", "safety_chain", "external"};
  for (unsigned bit = 0; bit < 8; ++bit) {
    HeatingSnapshot fault;
    fault.observe(0x8830, 1U << bit);
    text = render(fault);
    checkExposition(text.c_str(), 8, false);
    for (unsigned other = 0; other < 8; ++other) {
      const std::string line = std::string("km271_fault_active{component=\"boiler\",fault=\"") + boilerFaults[other] + "\"} " + (bit == other ? "1\n" : "0\n");
      expect(text, line.c_str());
    }
  }
  for (uint16_t address : {0x8001, 0x8113}) {
    const char *component = address == 0x8001 ? "heating_circuit_1" : "heating_circuit_2";
    const unsigned bits[] = {2, 3, 4, 6};
    const char *names[] = {"remote_communication", "remote_control", "flow_sensor", "external"};
    for (unsigned i = 0; i < 4; ++i) {
      HeatingSnapshot fault;
      fault.circuitEnabled[0] = fault.circuitEnabled[1] = true;
      fault.observe(address, 1U << bits[i]);
      text = render(fault);
      checkExposition(text.c_str(), 4, false);
      expect(text, (std::string("km271_fault_active{component=\"") + component + "\",fault=\"" + names[i] + "\"} 1\n").c_str());
    }
  }

  // Omit unusable unsigned temperature values and invalid pump commands.
  snapshot = fullSnapshot();
  snapshot.observe(0x882b, 255);
  snapshot.observe(0x8008, 101);
  snapshot.observe(0x811a, 255);
  snapshot.observe(0x893c, 255); // Signed outdoor temperatures: 0xff means -1 C.
  text = render(snapshot);
  absent(text, "km271_boiler_temperature_celsius");
  absent(text, "km271_heating_circuit_pump_ratio");
  expect(text, "km271_outside_temperature_celsius -1\n");

  for (uint8_t command : {0, 1, 4, 5}) {
    snapshot.observe(0x8832, command);
    expect(render(snapshot), command ? "km271_burner_active 1\n" : "km271_burner_active 0\n");
  }
  snapshot.observe(0x8832, 255);
  absent(render(snapshot), "km271_burner_active");

  HeatingSnapshot runtime;
  runtime.observe(0x8838, 255);
  assert(!runtime.burnerRuntimeValid);
  runtime.observe(0x8836, 0);
  assert(!runtime.burnerRuntimeValid);
  runtime.observe(0x8837, 0);
  expect(render(runtime), "km271_burner_runtime_seconds_total 15300\n");
  runtime.observe(0x8837, 1); // Do not expose a mixed total during carry.
  expect(render(runtime), "km271_burner_runtime_seconds_total 15300\n");
  runtime.observe(0x8838, 0);
  expect(render(runtime), "km271_burner_runtime_seconds_total 15360\n");
  runtime.observe(0x8838, 1); // Unchanged high/middle bytes need not be resent.
  expect(render(runtime), "km271_burner_runtime_seconds_total 15420\n");
  runtime.observe(0x8836, 255);
  runtime.observe(0x8837, 255);
  runtime.observe(0x8838, 255);
  expect(render(runtime), "km271_burner_runtime_seconds_total 1006632900\n");
  runtime.observe(0x8836, 0);
  runtime.observe(0x8837, 0);
  runtime.observe(0x8838, 0); // Controller reset/rollover becomes a counter reset.
  expect(render(runtime), "km271_burner_runtime_seconds_total 0\n");

  snapshot = fullSnapshot();
  strcpy(device, "Heizung \"A\"\\Keller\nSüd");
  text = render(snapshot, true);
  checkExposition(text.c_str(), 36, true);
  expect(text, "device=\"Heizung \\\"A\\\"\\\\Keller\\nSüd\"");
  memset(device, 'x', sizeof(device));
  char invalid[32] = {'!'};
  assert(metrics::heatingMetricsSize(snapshot, device, true) == 0);
  assert(metrics::formatHeatingMetrics(invalid, sizeof(invalid), snapshot, device, true) == 0 && invalid[0] == '\0');
  checkExposition(render(snapshot, false).c_str(), 36, false);

  // Maximum label expansion and all enabled metric families must fit together.
  for (auto &value : snapshot.values) {
    value = 254;
  }
  snapshot.values[HeatingSnapshot::OUTSIDE] = snapshot.values[HeatingSnapshot::OUTSIDE_DAMPED] = 128;
  snapshot.values[HeatingSnapshot::HC1_PUMP] = snapshot.values[HeatingSnapshot::HC2_PUMP] = 100;
  snapshot.values[HeatingSnapshot::BOILER_FAULTS] = snapshot.values[HeatingSnapshot::HC1_FAULTS] =
      snapshot.values[HeatingSnapshot::HC2_FAULTS] = snapshot.values[HeatingSnapshot::HOT_WATER_FAULTS] = 0;
  snapshot.values[HeatingSnapshot::BURNER] = 5;
  snapshot.burnerRuntimeMinutes = 0xffffff;
  metrics::HardwareSnapshot hardware;
  hardware.includeDeviceLabel = hardware.cpuUsageValid = hardware.wifiConnected = true;
  hardware.uptimeUs = hardware.sampleAgeUs = UINT64_MAX;
  hardware.heapSize = hardware.heapMinFree = hardware.heapLargestBlock = hardware.cpuFrequencyHz = UINT32_MAX;
  hardware.wifiRssi = INT32_MIN;
  memset(hardware.device, '"', sizeof(hardware.device) - 1);
  memset(hardware.firmwareVersion, '\\', sizeof(hardware.firmwareVersion) - 1);
  memset(hardware.chipModel, '\n', sizeof(hardware.chipModel) - 1);
  memcpy(device, hardware.device, sizeof(device));
  const size_t hardwareLength = metrics::hardwareMetricsSize(hardware);
  const size_t heatingLength = metrics::heatingMetricsSize(snapshot, device, true);
  assert(hardwareLength + heatingLength < metrics::RESPONSE_CAPACITY);
  std::vector<char> combined(hardwareLength + heatingLength + 1);
  assert(metrics::formatHardwareMetrics(combined.data(), combined.size(), hardware) == hardwareLength);
  assert(metrics::formatHeatingMetrics(combined.data() + hardwareLength, heatingLength + 1, snapshot, device, true) == heatingLength);
  checkExposition(combined.data(), 50, true);
  assert(strlen(combined.data()) == hardwareLength + heatingLength);

  // Every undersized output is rejected without writing outside its bounds.
  for (size_t capacity = 1; capacity <= heatingLength; ++capacity) {
    std::vector<char> guarded(capacity + 2, '!');
    assert(metrics::formatHeatingMetrics(guarded.data() + 1, capacity, snapshot, device, true) == 0);
    assert(guarded.front() == '!' && guarded.back() == '!' && guarded[1] == '\0');
  }
  printf("Heating metrics checks passed; full response with maximum labels: %zu / %zu bytes; heating snapshot: %zu bytes\n",
         hardwareLength + heatingLength, metrics::RESPONSE_CAPACITY, sizeof(HeatingSnapshot));
}
