#include <hardwareMetrics.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

namespace metrics {

namespace {

template <size_t N> bool escapeLabel(const char (&source)[N], char (&target)[2 * N]) {
  size_t length = 0;
  for (size_t i = 0; i < N; ++i) {
    const char value = source[i];
    if (value == '\0') {
      target[length] = '\0';
      return true;
    }
    if (value == '\\' || value == '"' || value == '\n') {
      target[length++] = '\\';
    }
    target[length++] = value == '\n' ? 'n' : value;
  }
  return false;
}

} // namespace

float cpuUsageRatio(uint32_t previousIdleUs, uint32_t currentIdleUs, uint32_t elapsedUs) {
  const uint32_t idleUs = currentIdleUs - previousIdleUs;
  if (elapsedUs == 0 || idleUs >= elapsedUs) {
    return 0.0f;
  }
  return 1.0f - static_cast<float>(idleUs) / elapsedUs;
}

static size_t renderHardwareMetrics(char *buffer, size_t capacity, const HardwareSnapshot &snapshot) {
  if (buffer) {
    buffer[0] = '\0';
  }
  char device[2 * sizeof(snapshot.device)] = {};
  char firmwareVersion[2 * sizeof(snapshot.firmwareVersion)];
  char chipModel[2 * sizeof(snapshot.chipModel)];
  if ((snapshot.includeDeviceLabel && !escapeLabel(snapshot.device, device)) || !escapeLabel(snapshot.firmwareVersion, firmwareVersion) ||
      !escapeLabel(snapshot.chipModel, chipModel)) {
    return 0;
  }
  const char *deviceOpen = snapshot.includeDeviceLabel ? "{device=\"" : "";
  const char *deviceClose = snapshot.includeDeviceLabel ? "\"}" : "";
  const char *attributeOpen = snapshot.includeDeviceLabel ? "device=\"" : "";
  const char *attributeClose = snapshot.includeDeviceLabel ? "\"," : "";

  size_t length = 0;
  bool overflow = false;
  auto append = [&](const char *format, ...) {
    if (overflow) {
      return;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer ? buffer + length : nullptr, buffer ? capacity - length : 0, format, args);
    va_end(args);
    if (written < 0 || (buffer && static_cast<size_t>(written) >= capacity - length)) {
      overflow = true;
      return;
    }
    length += static_cast<size_t>(written);
  };

  auto gauge = [&](const char *name, const char *help, uint32_t value) {
    append("# HELP %s %s\n# TYPE %s gauge\n%s%s%s%s %" PRIu32 "\n", name, help, name, name, deviceOpen, device, deviceClose, value);
  };

  append("# HELP km271_info Firmware and chip information.\n# TYPE km271_info gauge\n"
         "km271_info{%s%s%sfirmware_version=\"%s\",chip_model=\"%s\"} 1\n",
         attributeOpen, device, attributeClose, firmwareVersion, chipModel);
  append("# HELP km271_uptime_seconds Time since boot.\n# TYPE km271_uptime_seconds gauge\nkm271_uptime_seconds%s%s%s %.3f\n",
         deviceOpen, device, deviceClose, snapshot.uptimeUs / 1000000.0);
  append("# HELP km271_metrics_sample_age_seconds Age of the hardware snapshot.\n"
         "# TYPE km271_metrics_sample_age_seconds gauge\nkm271_metrics_sample_age_seconds%s%s%s %.3f\n",
         deviceOpen, device, deviceClose, snapshot.sampleAgeUs / 1000000.0);
  gauge("km271_heap_size_bytes", "Total internal heap.", snapshot.heapSize);
  gauge("km271_heap_used_bytes", "Allocated internal heap.", snapshot.heapSize - snapshot.heapFree);
  gauge("km271_heap_free_bytes", "Free internal heap.", snapshot.heapFree);
  gauge("km271_heap_min_free_bytes", "Minimum free internal heap since boot.", snapshot.heapMinFree);
  gauge("km271_heap_largest_free_block_bytes", "Largest free internal heap block.", snapshot.heapLargestBlock);
  gauge("km271_cpu_frequency_hertz", "CPU clock frequency.", snapshot.cpuFrequencyHz);

  if (snapshot.cpuUsageValid) {
    append("# HELP km271_cpu_usage_ratio Estimated busy fraction over the sampling interval.\n# TYPE km271_cpu_usage_ratio gauge\n");
    for (size_t core = 0; core < CPU_COUNT; ++core) {
      append("km271_cpu_usage_ratio{%s%s%score=\"%u\"} %.4f\n", attributeOpen, device, attributeClose, static_cast<unsigned>(core),
             static_cast<double>(snapshot.cpuUsage[core]));
    }
  }

  append("# HELP km271_network_connected Network interface has an IP address.\n# TYPE km271_network_connected gauge\n"
         "km271_network_connected{%s%s%sinterface=\"wifi\"} %u\nkm271_network_connected{%s%s%sinterface=\"ethernet\"} %u\n",
         attributeOpen, device, attributeClose, static_cast<unsigned>(snapshot.wifiConnected), attributeOpen, device, attributeClose,
         static_cast<unsigned>(snapshot.ethernetConnected));
  if (snapshot.wifiConnected) {
    append("# HELP km271_wifi_rssi_dbm WiFi received signal strength.\n# TYPE km271_wifi_rssi_dbm gauge\n"
           "km271_wifi_rssi_dbm%s%s%s %" PRId32 "\n", deviceOpen, device, deviceClose, snapshot.wifiRssi);
  }

  if (overflow) {
    if (buffer) {
      buffer[0] = '\0';
    }
    return 0;
  }
  return length;
}

size_t formatHardwareMetrics(char *buffer, size_t capacity, const HardwareSnapshot &snapshot) {
  return capacity == 0 ? 0 : renderHardwareMetrics(buffer, capacity, snapshot);
}

size_t hardwareMetricsSize(const HardwareSnapshot &snapshot) {
  return renderHardwareMetrics(nullptr, 0, snapshot);
}

} // namespace metrics
