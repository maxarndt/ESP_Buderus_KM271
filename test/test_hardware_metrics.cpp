#include <hardwareMetrics.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static size_t sampleSeparator(const std::string &line) {
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (quoted && line[i] == '\\') {
      ++i;
    } else if (line[i] == '"') {
      quoted = !quoted;
    } else if (!quoted && line[i] == ' ') {
      return i;
    }
  }
  return std::string::npos;
}

static void checkExposition(const char *text, size_t expectedSamples, bool expectDeviceLabel = true) {
  std::istringstream lines(text);
  std::map<std::string, unsigned> help;
  std::map<std::string, unsigned> types;
  std::set<std::string> samples;
  std::string line;
  while (std::getline(lines, line)) {
    if (line.compare(0, 7, "# HELP ") == 0) {
      const std::string name = line.substr(7, line.find(' ', 7) - 7);
      assert(++help[name] == 1);
    } else if (line.compare(0, 7, "# TYPE ") == 0) {
      const size_t separator = line.find(' ', 7);
      assert(line.substr(separator + 1) == "gauge");
      assert(++types[line.substr(7, separator - 7)] == 1);
    } else {
      const size_t separator = sampleSeparator(line);
      assert(separator != std::string::npos);
      const std::string series = line.substr(0, separator);
      const std::string name = series.substr(0, series.find('{'));
      assert(name.compare(0, 6, "km271_") == 0);
      assert((series.find("device=\"") != std::string::npos) == expectDeviceLabel);
      assert(series.find("{,", 0) == std::string::npos);
      assert(series.find(",}", 0) == std::string::npos);
      assert(series.find("{}", 0) == std::string::npos);
      if (name != "km271_info") {
        assert(series.find("firmware_version=") == std::string::npos);
        assert(series.find("chip_model=") == std::string::npos);
      }
      assert(help[name] == 1 && types[name] == 1);
      assert(samples.insert(series).second);
      char *end;
      const std::string number = line.substr(separator + 1);
      assert(isfinite(strtod(number.c_str(), &end)));
      assert(end != number.c_str() && *end == '\0');
    }
  }
  assert(samples.size() == expectedSamples);
  assert(text[strlen(text) - 1] == '\n');
}

int main() {
  using metrics::cpuUsageRatio;
  assert(fabs(cpuUsageRatio(100, 600, 1000) - 0.5f) < 0.00001f);
  assert(cpuUsageRatio(100, 100, 1000) == 1.0f);
  assert(cpuUsageRatio(100, 1100, 1000) == 0.0f);
  assert(cpuUsageRatio(100, 1101, 1000) == 0.0f);
  assert(cpuUsageRatio(100, 100, 0) == 0.0f);
  assert(fabs(cpuUsageRatio(UINT32_MAX - 99, 100, 1000) - 0.8f) < 0.00001f);

  metrics::HardwareSnapshot snapshot;
  strcpy(snapshot.device, "boiler");
  strcpy(snapshot.firmwareVersion, "v5.4.1");
  strcpy(snapshot.chipModel, "ESP32");
  snapshot.uptimeUs = UINT64_C(4294967296000); // beyond the 32-bit millis() rollover
  snapshot.sampleAgeUs = 125000;
  snapshot.heapSize = 300000;
  snapshot.heapFree = 125000;
  snapshot.heapMinFree = 100000;
  snapshot.heapLargestBlock = 64000;
  snapshot.cpuFrequencyHz = 240000000;
  snapshot.cpuUsageValid = true;
  snapshot.cpuUsage[0] = 0.125f;
  snapshot.cpuUsage[1] = 0.75f;
  snapshot.wifiConnected = true;
  snapshot.wifiRssi = -67;

  char buffer[metrics::RESPONSE_CAPACITY];
  assert(!snapshot.includeDeviceLabel);
  assert(metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot) > 0);
  checkExposition(buffer, 14, false);
  assert(metrics::hardwareMetricsSize(snapshot) == strlen(buffer));
  assert(strstr(buffer, "km271_heap_used_bytes 175000\n"));
  assert(strstr(buffer, "km271_uptime_seconds 4294967.296\n"));
  assert(strstr(buffer, "km271_info{firmware_version=\"v5.4.1\",chip_model=\"ESP32\"} 1\n"));
  assert(strstr(buffer, "km271_cpu_usage_ratio{core=\"0\"} 0.1250\n"));
  assert(strstr(buffer, "km271_network_connected{interface=\"wifi\"} 1\n"));
  assert(strstr(buffer, "km271_wifi_rssi_dbm -67\n"));

  snapshot.includeDeviceLabel = true;
  const size_t length = metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot);
  assert(length > 0 && length == strlen(buffer));
  assert(metrics::hardwareMetricsSize(snapshot) == length);
  checkExposition(buffer, 14);
  assert(strstr(buffer, "km271_info{device=\"boiler\",firmware_version=\"v5.4.1\",chip_model=\"ESP32\"} 1\n"));
  assert(strstr(buffer, "km271_uptime_seconds{device=\"boiler\"} 4294967.296\n"));
  assert(strstr(buffer, "km271_metrics_sample_age_seconds{device=\"boiler\"} 0.125\n"));
  assert(strstr(buffer, "km271_heap_used_bytes{device=\"boiler\"} 175000\n"));
  assert(strstr(buffer, "km271_cpu_frequency_hertz{device=\"boiler\"} 240000000\n"));
  assert(strstr(buffer, "km271_cpu_usage_ratio{device=\"boiler\",core=\"0\"} 0.1250\n"));
  assert(strstr(buffer, "km271_wifi_rssi_dbm{device=\"boiler\"} -67\n"));

  std::vector<char> exact(length + 1);
  assert(metrics::formatHardwareMetrics(exact.data(), exact.size(), snapshot) == length);
  for (size_t capacity = 1; capacity <= length; ++capacity) {
    std::vector<char> guarded(capacity + 2, '!');
    assert(metrics::formatHardwareMetrics(guarded.data() + 1, capacity, snapshot) == 0);
    assert(guarded[0] == '!' && guarded[capacity + 1] == '!');
    assert(guarded[1] == '\0');
  }
  assert(metrics::formatHardwareMetrics(nullptr, 0, snapshot) == 0);

  snapshot.cpuUsageValid = false;
  snapshot.wifiConnected = false;
  snapshot.ethernetConnected = true;
  assert(metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot) > 0);
  checkExposition(buffer, 11);
  assert(metrics::hardwareMetricsSize(snapshot) == strlen(buffer));
  assert(!strstr(buffer, "km271_cpu_usage_ratio"));
  assert(!strstr(buffer, "km271_wifi_rssi_dbm"));
  assert(strstr(buffer, "km271_network_connected{device=\"boiler\",interface=\"wifi\"} 0\n"));
  assert(strstr(buffer, "km271_network_connected{device=\"boiler\",interface=\"ethernet\"} 1\n"));

  strcpy(snapshot.device, "Boiler \"A\"\\Keller\nSüd");
  strcpy(snapshot.firmwareVersion, "version\"\\\n");
  strcpy(snapshot.chipModel, "chip\"\\\n");
  assert(metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot) > 0);
  checkExposition(buffer, 11);
  assert(strstr(buffer, "device=\"Boiler \\\"A\\\"\\\\Keller\\nSüd\""));
  assert(strstr(buffer, "firmware_version=\"version\\\"\\\\\\n\""));
  assert(strstr(buffer, "chip_model=\"chip\\\"\\\\\\n\""));

  snapshot.uptimeUs = UINT64_MAX;
  snapshot.sampleAgeUs = UINT64_MAX;
  snapshot.heapSize = UINT32_MAX;
  snapshot.heapFree = 0;
  snapshot.heapLargestBlock = UINT32_MAX;
  snapshot.cpuFrequencyHz = UINT32_MAX;
  snapshot.cpuUsageValid = true;
  snapshot.wifiConnected = true;
  snapshot.wifiRssi = INT32_MIN;
  memset(snapshot.device, '"', sizeof(snapshot.device) - 1);
  snapshot.device[sizeof(snapshot.device) - 1] = '\0';
  memset(snapshot.firmwareVersion, '\\', sizeof(snapshot.firmwareVersion) - 1);
  snapshot.firmwareVersion[sizeof(snapshot.firmwareVersion) - 1] = '\0';
  memset(snapshot.chipModel, '\n', sizeof(snapshot.chipModel) - 1);
  snapshot.chipModel[sizeof(snapshot.chipModel) - 1] = '\0';
  assert(metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot) > 0);
  checkExposition(buffer, 14);
  assert(metrics::hardwareMetricsSize(snapshot) == strlen(buffer));

  printf("Hardware metrics checks passed; maximum fixture response: %zu / %zu bytes\n", strlen(buffer), sizeof(buffer));

  memset(snapshot.device, 'x', sizeof(snapshot.device)); // reject an unterminated source
  assert(metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot) == 0);
  assert(metrics::hardwareMetricsSize(snapshot) == 0);
  assert(buffer[0] == '\0');

  // Turning off the label must also ignore an unused device name.
  snapshot.includeDeviceLabel = false;
  assert(metrics::formatHardwareMetrics(buffer, sizeof(buffer), snapshot) > 0);
  assert(metrics::hardwareMetricsSize(snapshot) == strlen(buffer));
  checkExposition(buffer, 14, false);
}
