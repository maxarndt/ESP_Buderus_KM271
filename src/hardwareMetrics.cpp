#include <hardwareMetrics.h>
#include <metricsText.h>

#include <inttypes.h>

namespace metrics {

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
  if ((snapshot.includeDeviceLabel && !text::escapeLabel(snapshot.device, device)) || !text::escapeLabel(snapshot.firmwareVersion, firmwareVersion) ||
      !text::escapeLabel(snapshot.chipModel, chipModel)) {
    return 0;
  }
  const char *deviceOpen = snapshot.includeDeviceLabel ? "{device=\"" : "";
  const char *deviceClose = snapshot.includeDeviceLabel ? "\"}" : "";
  const char *attributeOpen = snapshot.includeDeviceLabel ? "device=\"" : "";
  const char *attributeClose = snapshot.includeDeviceLabel ? "\"," : "";

  text::Writer writer(buffer, capacity);

  auto gauge = [&](const char *name, const char *help, uint32_t value) {
    writer.append("# HELP %s %s\n# TYPE %s gauge\n%s%s%s%s %" PRIu32 "\n", name, help, name, name, deviceOpen, device, deviceClose, value);
  };

  writer.append("# HELP km271_info Firmware and chip information.\n# TYPE km271_info gauge\n"
         "km271_info{%s%s%sfirmware_version=\"%s\",chip_model=\"%s\"} 1\n",
         attributeOpen, device, attributeClose, firmwareVersion, chipModel);
  writer.append("# HELP km271_uptime_seconds Time since boot.\n# TYPE km271_uptime_seconds gauge\nkm271_uptime_seconds%s%s%s %.3f\n",
         deviceOpen, device, deviceClose, snapshot.uptimeUs / 1000000.0);
  writer.append("# HELP km271_metrics_sample_age_seconds Age of the hardware snapshot.\n"
         "# TYPE km271_metrics_sample_age_seconds gauge\nkm271_metrics_sample_age_seconds%s%s%s %.3f\n",
         deviceOpen, device, deviceClose, snapshot.sampleAgeUs / 1000000.0);
  gauge("km271_heap_size_bytes", "Total internal heap.", snapshot.heapSize);
  gauge("km271_heap_used_bytes", "Allocated internal heap.", snapshot.heapSize - snapshot.heapFree);
  gauge("km271_heap_free_bytes", "Free internal heap.", snapshot.heapFree);
  gauge("km271_heap_min_free_bytes", "Minimum free internal heap since boot.", snapshot.heapMinFree);
  gauge("km271_heap_largest_free_block_bytes", "Largest free internal heap block.", snapshot.heapLargestBlock);
  gauge("km271_cpu_frequency_hertz", "CPU clock frequency.", snapshot.cpuFrequencyHz);

  if (snapshot.cpuUsageValid) {
    writer.append("# HELP km271_cpu_usage_ratio Estimated busy fraction over the sampling interval.\n# TYPE km271_cpu_usage_ratio gauge\n");
    for (size_t core = 0; core < CPU_COUNT; ++core) {
      writer.append("km271_cpu_usage_ratio{%s%s%score=\"%u\"} %.4f\n", attributeOpen, device, attributeClose, static_cast<unsigned>(core),
             static_cast<double>(snapshot.cpuUsage[core]));
    }
  }

  writer.append("# HELP km271_network_connected Network interface has an IP address.\n# TYPE km271_network_connected gauge\n"
         "km271_network_connected{%s%s%sinterface=\"wifi\"} %u\nkm271_network_connected{%s%s%sinterface=\"ethernet\"} %u\n",
         attributeOpen, device, attributeClose, static_cast<unsigned>(snapshot.wifiConnected), attributeOpen, device, attributeClose,
         static_cast<unsigned>(snapshot.ethernetConnected));
  if (snapshot.wifiConnected) {
    writer.append("# HELP km271_wifi_rssi_dbm WiFi received signal strength.\n# TYPE km271_wifi_rssi_dbm gauge\n"
           "km271_wifi_rssi_dbm%s%s%s %" PRId32 "\n", deviceOpen, device, deviceClose, snapshot.wifiRssi);
  }

  return writer.finish();
}

size_t formatHardwareMetrics(char *buffer, size_t capacity, const HardwareSnapshot &snapshot) {
  return capacity == 0 ? 0 : renderHardwareMetrics(buffer, capacity, snapshot);
}

size_t hardwareMetricsSize(const HardwareSnapshot &snapshot) {
  return renderHardwareMetrics(nullptr, 0, snapshot);
}

} // namespace metrics
