#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

namespace metrics {
namespace text {

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

// The same writer is used for sizing (buffer == nullptr) and rendering.
class Writer {
public:
  Writer(char *buffer, size_t capacity) : buffer(buffer), capacity(capacity) {}

  void append(const char *format, ...) __attribute__((format(printf, 2, 3))) {
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
  }

  size_t finish() {
    if (buffer) {
      buffer[overflow ? 0 : length] = '\0';
    }
    return overflow ? 0 : length;
  }

private:
  char *buffer;
  size_t capacity;
  size_t length = 0;
  bool overflow = false;
};

} // namespace text
} // namespace metrics
