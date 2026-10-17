#pragma once

#include <stddef.h>
#include <stdint.h>

namespace metrics {

// Hardware plus heating, with a full-length escaped device label on every series.
// Responses allocate their actual size, not this upper bound.
constexpr size_t RESPONSE_CAPACITY = 20480;
constexpr size_t CPU_COUNT = 2;

struct HardwareSnapshot {
  char device[128] = {};
  char firmwareVersion[32] = {};
  char chipModel[32] = {};
  uint64_t uptimeUs = 0;
  uint64_t sampleAgeUs = 0;
  uint32_t heapSize = 0;
  uint32_t heapFree = 0;
  uint32_t heapMinFree = 0;
  uint32_t heapLargestBlock = 0;
  uint32_t cpuFrequencyHz = 0;
  float cpuUsage[CPU_COUNT] = {};
  bool cpuUsageValid = false;
  bool includeDeviceLabel = false;
  bool wifiConnected = false;
  bool ethernetConnected = false;
  int32_t wifiRssi = 0;
};

// The unsigned subtraction also handles a wrap of the SDK's 32-bit idle counter.
float cpuUsageRatio(uint32_t previousIdleUs, uint32_t currentIdleUs, uint32_t elapsedUs);

// Returns zero on overflow; a partial exposition must never be served.
size_t formatHardwareMetrics(char *buffer, size_t capacity, const HardwareSnapshot &snapshot);
// Payload size excluding the terminator; returns zero for invalid labels.
size_t hardwareMetricsSize(const HardwareSnapshot &snapshot);

} // namespace metrics
