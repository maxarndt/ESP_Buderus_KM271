#include <metrics.h>

#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <WiFi.h>
#include <config.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <hardwareMetrics.h>
#include <atomic>
#include <memory>
#include <new>

#ifndef METRICS_PORT
#define METRICS_PORT 9100
#endif

namespace {

constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;
AsyncWebServer server(METRICS_PORT);
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
bool started = false;
bool routesReady = false;
bool responseInFlight = false;
uint32_t lastSampleMs = 0;
std::atomic<bool> otaActive{false};
std::atomic<bool> enabled{false};

struct CachedState {
  metrics::HardwareSnapshot hardware;
  s_cfg_auth auth;
};

CachedState state;

void sampleHardware(bool resetCpu = false) {
  CachedState next;
  next.auth = config.auth;
  auto &snapshot = next.hardware;
  snapshot.includeDeviceLabel = config.metrics.device_label;
  if (snapshot.includeDeviceLabel) {
    const char *device = config.wifi.hostname[0] ? config.wifi.hostname : config.eth.hostname;
    snprintf(snapshot.device, sizeof(snapshot.device), "%s", device[0] ? device : "ESP-Buderus-KM271");
  }
  snprintf(snapshot.firmwareVersion, sizeof(snapshot.firmwareVersion), "%s", VERSION);
  snprintf(snapshot.chipModel, sizeof(snapshot.chipModel), "%s", ESP.getChipModel());
  snapshot.uptimeUs = esp_timer_get_time();

  multi_heap_info_t heap;
  heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL);
  snapshot.heapSize = ESP.getHeapSize();
  snapshot.heapFree = heap.total_free_bytes;
  snapshot.heapMinFree = heap.minimum_free_bytes;
  snapshot.heapLargestBlock = heap.largest_free_block;
  snapshot.cpuFrequencyHz = ESP.getCpuFreqMHz() * 1000000UL;
  snapshot.wifiConnected = WiFi.isConnected();
  snapshot.ethernetConnected = ETH.hasIP();
  if (snapshot.wifiConnected) {
    snapshot.wifiRssi = WiFi.RSSI();
  }

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP
  static_assert(portNUM_PROCESSORS == metrics::CPU_COUNT, "Hardware metrics expect a dual-core ESP32");
  static uint64_t previousSampleUs = 0;
  static uint32_t previousIdleUs[metrics::CPU_COUNT] = {};
  const uint64_t elapsedUs = snapshot.uptimeUs - previousSampleUs;
  snapshot.cpuUsageValid = !resetCpu && previousSampleUs != 0 && elapsedUs > 0 && elapsedUs <= UINT32_MAX;
  for (size_t core = 0; core < metrics::CPU_COUNT; ++core) {
    const uint32_t idleUs = ulTaskGetIdleRunTimeCounterForCore(core);
    if (snapshot.cpuUsageValid) {
      snapshot.cpuUsage[core] = metrics::cpuUsageRatio(previousIdleUs[core], idleUs, static_cast<uint32_t>(elapsedUs));
    }
    previousIdleUs[core] = idleUs;
  }
  previousSampleUs = snapshot.uptimeUs;
#endif

  // Copy only small POD values under the lock; no formatting or I/O here.
  portENTER_CRITICAL(&stateMux);
  state = next;
  portEXIT_CRITICAL(&stateMux);
}

// Allocate exactly the rendered size, bounded by RESPONSE_CAPACITY. A new
// sample cannot change a response that is still being transmitted.
class MetricsResponse : public AsyncAbstractResponse {
public:
  explicit MetricsResponse(const metrics::HardwareSnapshot &snapshot) {
    _code = 200;
    _contentType = "text/plain; version=0.0.4; charset=utf-8";
    const size_t length = metrics::hardwareMetricsSize(snapshot);
    if (length == 0 || length >= metrics::RESPONSE_CAPACITY) {
      _code = 500;
      errorBody = "Metrics response exceeds buffer\n";
    } else {
      body.reset(new (std::nothrow) char[length + 1]);
      if (!body) {
        _code = 503;
        errorBody = "Insufficient memory for metrics\n";
      } else {
        _contentLength = metrics::formatHardwareMetrics(body.get(), length + 1, snapshot);
        if (_contentLength != length) {
          _code = 500;
          errorBody = "Metrics formatting failed\n";
        }
      }
    }
    if (errorBody) {
      body.reset();
      _contentType = "text/plain";
      _contentLength = strlen(errorBody);
    }
    addHeader("Cache-Control", "no-store");
    addHeader("Connection", "close");
  }

  ~MetricsResponse() override {
    portENTER_CRITICAL(&stateMux);
    responseInFlight = false;
    portEXIT_CRITICAL(&stateMux);
  }

  bool _sourceValid() const override { return true; }

  size_t _fillBuffer(uint8_t *buffer, size_t maxLength) override {
    const size_t remaining = _contentLength - offset;
    const size_t count = remaining < maxLength ? remaining : maxLength;
    memcpy(buffer, (errorBody ? errorBody : body.get()) + offset, count);
    offset += count;
    return count;
  }

private:
  std::unique_ptr<char[]> body;
  const char *errorBody = nullptr;
  size_t offset = 0;
};

void handleMetrics(AsyncWebServerRequest *request) {
  request->client()->setRxTimeout(3);
  request->client()->setAckTimeout(3000);

  if (!enabled.load(std::memory_order_relaxed)) {
    request->send(503, "text/plain", "Metrics disabled\n");
    return;
  }

  CachedState current;
  portENTER_CRITICAL(&stateMux);
  current = state;
  portEXIT_CRITICAL(&stateMux);

  if (current.auth.enable && !request->authenticate(current.auth.user, current.auth.password)) {
    request->requestAuthentication(AsyncAuthType::AUTH_BASIC, "ESP metrics");
    return;
  }
  if (otaActive.load(std::memory_order_relaxed)) {
    request->send(503, "text/plain", "OTA in progress\n");
    return;
  }

  portENTER_CRITICAL(&stateMux);
  const bool busy = responseInFlight;
  if (!busy) {
    responseInFlight = true;
  }
  portEXIT_CRITICAL(&stateMux);
  if (busy) {
    request->send(503, "text/plain", "Metrics response already in progress\n");
    return;
  }

  current.hardware.sampleAgeUs = esp_timer_get_time() - current.hardware.uptimeUs;
  auto *response = new (std::nothrow) MetricsResponse(current.hardware);
  if (response == nullptr) {
    portENTER_CRITICAL(&stateMux);
    responseInFlight = false;
    portEXIT_CRITICAL(&stateMux);
    request->client()->close(true);
    return;
  }
  request->send(response);
}

} // namespace

void metricsSetup() {
  if (setupMode || routesReady) {
    return;
  }
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404, "text/plain", "Not found\n"); });
  routesReady = true;
  metricsCyclic();
}

void metricsCyclic() {
  if (!routesReady) {
    return;
  }

  const bool shouldRun = config.metrics.enable && !setupMode;
  if (shouldRun != started) {
    if (shouldRun) {
      if (otaActive.load(std::memory_order_relaxed)) {
        return;
      }
      sampleHardware(true);
      lastSampleMs = millis();
      enabled.store(true, std::memory_order_relaxed);
      server.begin();
    } else {
      enabled.store(false, std::memory_order_relaxed);
      server.end();
    }
    started = shouldRun;
  }
  if (!started) {
    return;
  }

  const uint32_t now = millis();
  if (!otaActive.load(std::memory_order_relaxed) && static_cast<uint32_t>(now - lastSampleMs) >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    sampleHardware();
  }
}

void metricsSetOtaActive(bool active) {
  otaActive.store(active, std::memory_order_relaxed);
}
