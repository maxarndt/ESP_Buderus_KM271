# Hardware metrics

The ESP serves `GET http://<ESP-IP>:9100/metrics` using Prometheus text exposition
format 0.0.4. No MQTT broker, OTLP exporter, or conversion service is required.
The listener uses the existing AsyncTCP task; it does not create another task.
It is available over WiFi or Ethernet in normal operation, and is not started
in setup mode. Enable **Prometheus metrics** in the WebUI settings first.
The switch takes effect without a reboot and is saved as `metrics.enable` in
`config.json`. Metrics default to disabled, also when loading older configurations
without this field. Disabling stops both the listener and hardware sampling;
an already transmitting response can finish.

The dedicated port avoids changes to the external EspWebUI library, which keeps
its port-80 server private. To change the metrics port, add `-D METRICS_PORT=9101`
to the existing `build_flags` in `platformio.ini`. Choose a port other than 80.

## Metrics

All metrics are gauges. Heap values describe internal heap, not total physical
RAM or PSRAM. By default, metrics do not carry a `device` label. The separate
**Add device name as label** checkbox in the Prometheus settings enables it on
every sample, including `km271_info`. This is persisted as
`metrics.device_label`, defaults to `false` for new and older configs,
and takes effect with the next sample (within five seconds, without a reboot).

When enabled, `device` is taken from the configured WiFi hostname.
This name is also used when the ESP is connected through Ethernet,
so changing interfaces does not change the label. If the WiFi hostname is empty,
the Ethernet hostname is used, falling back to `ESP-Buderus-KM271` if both are
empty. Set a distinct hostname when using multiple devices.

Firmware version and chip model are labels on `km271_info` only. Prometheus /
VictoriaMetrics additionally attach their normal `job` and `instance` labels.
Quotes, backslashes, and newlines in label values are escaped.

Default output, for example:

```text
km271_heap_free_bytes 125000
km271_cpu_usage_ratio{core="0"} 0.1250
km271_info{firmware_version="v5.4.1",chip_model="ESP32"} 1
```

With the device-label checkbox enabled:

```text
km271_heap_free_bytes{device="heizung"} 125000
km271_info{device="heizung",firmware_version="v5.4.1",chip_model="ESP32"} 1
```

All names use the `km271_` prefix. The labels shown in the table are always
present for their respective metrics; `device` is added only when enabled.

| Metric | Meaning |
|---|---|
| `km271_info{firmware_version="...",chip_model="..."}` | Constant 1; firmware and chip metadata |
| `km271_uptime_seconds` | Time since boot, using the 64-bit ESP timer |
| `km271_metrics_sample_age_seconds` | Time since the main loop last sampled hardware |
| `km271_heap_size_bytes` | Total internal heap |
| `km271_heap_used_bytes` | Total internal heap minus free heap |
| `km271_heap_free_bytes` | Free internal heap |
| `km271_heap_min_free_bytes` | Minimum free internal heap since boot |
| `km271_heap_largest_free_block_bytes` | Largest contiguous free internal heap block |
| `km271_cpu_frequency_hertz` | CPU clock frequency |
| `km271_cpu_usage_ratio{core="0"}` / `{core="1"}` | Estimated busy fraction per core, from 0 to 1 |
| `km271_network_connected{interface="wifi"}` / `{interface="ethernet"}` | Interface has an IP address, 0 or 1 |
| `km271_wifi_rssi_dbm` | WiFi signal strength; omitted when WiFi is disconnected |

CPU utilization is calculated from differences in the existing FreeRTOS idle
runtime counters over each sampling interval. It is an estimate of scheduler
activity, not a precise hardware profiler. The pinned SDK already enables these
counters with the ESP timer. CPU utilization is omitted until two samples are
available, or when the SDK does not provide the required counters. Unsigned
subtraction handles 32-bit idle counter rollover. Intervals longer than a full
counter period are discarded.

For a percentage in Grafana, use `100 * km271_cpu_usage_ratio`. Monitor the largest
free block alongside free heap to detect fragmentation. A rising
`km271_metrics_sample_age_seconds` indicates delayed sampling even if HTTP still
responds. VictoriaMetrics supplies the scrape timestamp; NTP is not required.

## VictoriaMetrics configuration

Single-node VictoriaMetrics can scrape the endpoint itself. Add a job to the
configuration passed with `-promscrape.config`:

```yaml
scrape_configs:
  - job_name: km271_hardware
    scrape_interval: 30s
    scrape_timeout: 5s
    metrics_path: /metrics
    static_configs:
      - targets: ['192.168.1.50:9100'] # replace with the ESP address
    basic_auth:
      username: '<WebUI username>'
      password: '<WebUI password>'
```

The metrics endpoint follows `config.auth.enable` and uses the configured
WebUI credentials with HTTP Basic Auth. Remove `basic_auth` if authentication
is disabled. WebUI session cookies are not used. Authentication changes become
effective with the next sample, normally within five seconds.

Check the endpoint with:

```sh
curl --fail --user '<WebUI username>' http://192.168.1.50:9100/metrics
```

`curl` prompts for the password. Omit `--user` if authentication is disabled.
The endpoint returns `401` for missing/incorrect credentials and `404` for
unknown paths. OTA and overlapping metrics responses return `503`, which
appears as a failed scrape rather than a successful response containing old data.

See [VictoriaMetrics scrape configuration](https://docs.victoriametrics.com/victoriametrics/scrape_config_examples/).

## Resource bounds

- The main loop refreshes a numeric snapshot every five seconds. Sampling does
  not query the heating controller or perform network I/O.
- HTTP requests copy that snapshot under a short lock, then format and send it
  asynchronously. A response owns a buffer sized to the actual payload, capped
  at 6 KiB including the terminator. This covers the maximum escaped label sizes;
  shorter names allocate less memory. A new sample cannot overwrite a response
  that is still being transmitted.
- Only one metrics response is allowed in flight. This bounds the metrics body
  allocation; AsyncTCP and HTTP still have their own per-connection allocations.
- Response ACK timeout is three seconds. Connections close after the response.
- There are no outbound requests, retry queues, flash writes, JSON documents,
  full task listings, or per-task stack scans. Sampling pauses during OTA.

These bounds do not replace measurements on the device, especially when testing
WebUI, MQTT, and OTA concurrently.

## Validation

Build the firmware with `pio run -e esp32`. The formatting and CPU calculation
checks also run on the host without ESP hardware or additional libraries:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iinclude src/hardwareMetrics.cpp test/test_hardware_metrics.cpp \
  -o /tmp/km271-hardware-metrics-test
/tmp/km271-hardware-metrics-test
```

The checks cover units and values, optional metrics, unique series and metadata,
64-bit uptime, CPU counter rollover, enabled/disabled device labels, escaped and maximum-length labels, exact
payload sizing, and all undersized output buffers.

Before deploying broadly, check these cases on an ESP:

1. Enable metrics in the WebUI, reload and reboot to confirm persistence, then
   disable and re-enable without a reboot. Port 9100 should stop accepting new
   connections while disabled. Test importing an older config without `metrics`.
   Also verify the device-label checkbox defaults to off, persists when changed,
   and adds/removes `device` from all metrics (including `km271_info`).
2. Scrape with authentication enabled and disabled; try incorrect credentials.
3. Verify heap accounting, CPU ratios between 0 and 1, and omission of RSSI when
   using Ethernet with WiFi disconnected.
4. Scrape repeatedly while using WebUI and MQTT; disconnect a client during a
   response, then confirm later scrapes succeed.
5. Perform OTA and verify that metrics recover afterward; check the setup AP
   does not expose port 9100.
6. Compare free/minimum heap, largest free block, and KM271 behavior over an
   extended run with and without scraping.
