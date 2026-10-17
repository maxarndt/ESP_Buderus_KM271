#include <heatingMetrics.h>
#include <metricsText.h>

#include <inttypes.h>

namespace metrics {

void HeatingSnapshot::observe(uint16_t address, uint8_t value) {
  Field field;
  switch (address) {
  case 0x893c: field = OUTSIDE; break;
  case 0x893d: field = OUTSIDE_DAMPED; break;
  case 0x882a: field = BOILER_TARGET; break;
  case 0x882b: field = BOILER_ACTUAL; break;
  case 0x8002: field = HC1_TARGET; break;
  case 0x8003: field = HC1_ACTUAL; break;
  case 0x8008: field = HC1_PUMP; break;
  case 0x8001: field = HC1_FAULTS; break;
  case 0x8114: field = HC2_TARGET; break;
  case 0x8115: field = HC2_ACTUAL; break;
  case 0x811a: field = HC2_PUMP; break;
  case 0x8113: field = HC2_FAULTS; break;
  case 0x8426: field = HOT_WATER_TARGET; break;
  case 0x8427: field = HOT_WATER_ACTUAL; break;
  case 0x8429: field = HOT_WATER_PUMPS; break;
  case 0x8424: field = HOT_WATER_FAULTS; break;
  case 0x8830: field = BOILER_FAULTS; break;
  case 0x8832: field = BURNER; break;
  case 0x8836: field = RUNTIME_HIGH; break;
  case 0x8837: field = RUNTIME_MIDDLE; break;
  case 0x8838: field = RUNTIME_LOW; break;
  default: return;
  }
  values[field] = value;
  received |= UINT32_C(1) << field;

  // Like the existing decoder, commit subsequent totals on the low-byte
  // telegram, after any changed high/middle bytes have been received.
  if ((field == RUNTIME_LOW || (!burnerRuntimeValid && field >= RUNTIME_HIGH)) &&
      has(RUNTIME_HIGH) && has(RUNTIME_MIDDLE) && has(RUNTIME_LOW)) {
    burnerRuntimeMinutes = (static_cast<uint32_t>(values[RUNTIME_HIGH]) << 16) |
                           (static_cast<uint32_t>(values[RUNTIME_MIDDLE]) << 8) | values[RUNTIME_LOW];
    burnerRuntimeValid = true;
  }
}

namespace {

using Field = HeatingSnapshot::Field;

bool sensorFault(const HeatingSnapshot &snapshot, Field field, uint8_t mask) {
  return snapshot.has(field) && (snapshot.values[field] & mask) != 0;
}

size_t renderHeatingMetrics(char *buffer, size_t capacity, const HeatingSnapshot &snapshot,
                            const char (&deviceName)[128], bool includeDeviceLabel) {
  if (buffer) {
    buffer[0] = '\0';
  }
  char device[2 * sizeof(deviceName)] = {};
  if (includeDeviceLabel && !text::escapeLabel(deviceName, device)) {
    return 0;
  }
  const char *deviceOpen = includeDeviceLabel ? "{device=\"" : "";
  const char *deviceClose = includeDeviceLabel ? "\"}" : "";
  const char *attributeOpen = includeDeviceLabel ? "device=\"" : "";
  const char *attributeClose = includeDeviceLabel ? "\"," : "";
  text::Writer writer(buffer, capacity);
  auto metadata = [&](const char *name, const char *help, const char *type = "gauge") {
    writer.append("# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
  };
  auto temperature = [&](Field field, const char *name, const char *help, bool valid = true, bool signedValue = false) {
    if (!snapshot.has(field) || !valid || (!signedValue && snapshot.values[field] == 0xff)) {
      return;
    }
    const int raw = snapshot.values[field];
    const int value = signedValue && raw >= 128 ? raw - 256 : raw;
    metadata(name, help);
    writer.append("%s%s%s%s %d\n", name, deviceOpen, device, deviceClose, value);
  };

  temperature(Field::OUTSIDE, "km271_outside_temperature_celsius", "Outside temperature.", true, true);
  temperature(Field::OUTSIDE_DAMPED, "km271_outside_damped_temperature_celsius", "Damped outside temperature.", true, true);
  temperature(Field::BOILER_ACTUAL, "km271_boiler_temperature_celsius", "Measured boiler temperature.",
              !sensorFault(snapshot, Field::BOILER_FAULTS, 1 << 1));
  temperature(Field::BOILER_TARGET, "km271_boiler_target_temperature_celsius", "Boiler temperature setpoint.");

  const Field targets[] = {Field::HC1_TARGET, Field::HC2_TARGET};
  const Field actuals[] = {Field::HC1_ACTUAL, Field::HC2_ACTUAL};
  const Field pumps[] = {Field::HC1_PUMP, Field::HC2_PUMP};
  const Field faults[] = {Field::HC1_FAULTS, Field::HC2_FAULTS};
  const char *flowNames[] = {"km271_heating_circuit_flow_temperature_celsius", "km271_heating_circuit_flow_target_temperature_celsius"};
  const char *flowHelp[] = {"Measured heating circuit flow temperature.", "Heating circuit flow temperature setpoint."};
  for (unsigned kind = 0; kind < 2; ++kind) {
    bool header = false;
    for (unsigned circuit = 0; circuit < 2; ++circuit) {
      const Field field = kind == 0 ? actuals[circuit] : targets[circuit];
      if (!snapshot.circuitEnabled[circuit] || !snapshot.has(field) || snapshot.values[field] == 0xff ||
          (kind == 0 && sensorFault(snapshot, faults[circuit], 1 << 4))) {
        continue;
      }
      if (!header) {
        metadata(flowNames[kind], flowHelp[kind]);
        header = true;
      }
      writer.append("%s{%s%s%scircuit=\"%u\"} %u\n", flowNames[kind], attributeOpen, device, attributeClose,
                    circuit + 1, static_cast<unsigned>(snapshot.values[field]));
    }
  }
  bool pumpHeader = false;
  for (unsigned circuit = 0; circuit < 2; ++circuit) {
    if (!snapshot.circuitEnabled[circuit] || !snapshot.has(pumps[circuit]) || snapshot.values[pumps[circuit]] > 100) {
      continue;
    }
    if (!pumpHeader) {
      metadata("km271_heating_circuit_pump_ratio", "Reported heating circuit pump command, from 0 to 1; not electrical power.");
      pumpHeader = true;
    }
    writer.append("km271_heating_circuit_pump_ratio{%s%s%scircuit=\"%u\"} %.2f\n", attributeOpen, device, attributeClose,
                  circuit + 1, snapshot.values[pumps[circuit]] / 100.0);
  }

  if (snapshot.hotWaterEnabled) {
    temperature(Field::HOT_WATER_ACTUAL, "km271_hot_water_temperature_celsius", "Measured hot water temperature.",
                !sensorFault(snapshot, Field::HOT_WATER_FAULTS, 1 << 5));
    temperature(Field::HOT_WATER_TARGET, "km271_hot_water_target_temperature_celsius", "Hot water temperature setpoint.");
    if (snapshot.has(Field::HOT_WATER_PUMPS)) {
      metadata("km271_hot_water_pump_active", "Reported hot water pump state, 0 or 1.");
      const char *names[] = {"charging", "circulation"};
      for (unsigned bit = 0; bit < 2; ++bit) {
        writer.append("km271_hot_water_pump_active{%s%s%spump=\"%s\"} %u\n", attributeOpen, device, attributeClose,
                      names[bit], (snapshot.values[Field::HOT_WATER_PUMPS] >> bit) & 1U);
      }
    }
  }

  if (snapshot.has(Field::BURNER) && (snapshot.values[Field::BURNER] & ~0x05U) == 0) {
    metadata("km271_burner_active", "Reported burner command for stage 1 or 2, 0 or 1; not a flame sensor.");
    writer.append("km271_burner_active%s%s%s %u\n", deviceOpen, device, deviceClose, snapshot.values[Field::BURNER] ? 1U : 0U);
  }
  if (snapshot.burnerRuntimeValid) {
    metadata("km271_burner_runtime_seconds_total", "Controller burner operating time in seconds, with one-minute resolution.", "counter");
    writer.append("km271_burner_runtime_seconds_total%s%s%s %" PRIu64 "\n", deviceOpen, device, deviceClose,
                  static_cast<uint64_t>(snapshot.burnerRuntimeMinutes) * 60);
  }

  bool faultHeader = false;
  auto fault = [&](Field field, unsigned bit, const char *component, const char *name) {
    if (!snapshot.has(field)) {
      return;
    }
    if (!faultHeader) {
      metadata("km271_fault_active", "Reported controller fault flag, 0 or 1.");
      faultHeader = true;
    }
    writer.append("km271_fault_active{%s%s%scomponent=\"%s\",fault=\"%s\"} %u\n", attributeOpen, device, attributeClose,
                  component, name, (snapshot.values[field] >> bit) & 1U);
  };
  const char *boilerFaults[] = {"burner", "boiler_sensor", "auxiliary_sensor", "stays_cold", "exhaust_sensor", "exhaust_limit", "safety_chain", "external"};
  for (unsigned bit = 0; bit < 8; ++bit) {
    fault(Field::BOILER_FAULTS, bit, "boiler", boilerFaults[bit]);
  }
  for (unsigned circuit = 0; circuit < 2; ++circuit) {
    if (snapshot.circuitEnabled[circuit]) {
      const char *component = circuit == 0 ? "heating_circuit_1" : "heating_circuit_2";
      fault(faults[circuit], 2, component, "remote_communication");
      fault(faults[circuit], 3, component, "remote_control");
      fault(faults[circuit], 4, component, "flow_sensor");
      fault(faults[circuit], 6, component, "external");
    }
  }
  if (snapshot.hotWaterEnabled) {
    fault(Field::HOT_WATER_FAULTS, 4, "hot_water", "disinfection");
    fault(Field::HOT_WATER_FAULTS, 5, "hot_water", "temperature_sensor");
    fault(Field::HOT_WATER_FAULTS, 6, "hot_water", "stays_cold");
    fault(Field::HOT_WATER_FAULTS, 7, "hot_water", "anode");
  }
  return writer.finish();
}

} // namespace

size_t formatHeatingMetrics(char *buffer, size_t capacity, const HeatingSnapshot &snapshot,
                            const char (&device)[128], bool includeDeviceLabel) {
  return capacity == 0 ? 0 : renderHeatingMetrics(buffer, capacity, snapshot, device, includeDeviceLabel);
}

size_t heatingMetricsSize(const HeatingSnapshot &snapshot, const char (&device)[128], bool includeDeviceLabel) {
  return renderHeatingMetrics(nullptr, 0, snapshot, device, includeDeviceLabel);
}

} // namespace metrics
