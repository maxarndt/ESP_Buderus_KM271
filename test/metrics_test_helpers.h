#pragma once

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
      assert(line.substr(separator + 1) == (line.substr(7, separator - 7) == "km271_burner_runtime_seconds_total" ? "counter" : "gauge"));
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
  assert(expectedSamples == 0 || text[strlen(text) - 1] == '\n');
}

