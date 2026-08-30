#ifndef KATAGO_TELEMETRY_H
#define KATAGO_TELEMETRY_H

#include <chrono>
#include "katago_api.h"

namespace KataGoTelemetry {

// The callback is process-wide because search spans can be emitted by worker
// threads that do not have a KataGoEngine handle. Access goes through these
// functions so installing or removing a callback is race-free.
void setCallback(KataGoTelemetryCallback callback) noexcept;
KataGoTelemetryCallback getCallback() noexcept;

}

struct TraceSpan {
  const char* name;
  std::chrono::steady_clock::time_point start;
  
  TraceSpan(const char* n) : name(n), start(std::chrono::steady_clock::now()) {}
  
  ~TraceSpan() noexcept {
    KataGoTelemetryCallback callback = KataGoTelemetry::getCallback();
    if(callback != nullptr) {
      auto end = std::chrono::steady_clock::now();
      int64_t dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
      // Telemetry must never change engine behavior. In particular, a throwing
      // C++ host callback must not terminate the process from this destructor.
      try {
        callback(name, dur);
      } catch(...) {}
    }
  }
};

#define TRACE_SPAN_CONCAT(a, b) a ## b
#define TRACE_SPAN_MACRO(name, line) TraceSpan TRACE_SPAN_CONCAT(__span_, line)(name)
#define TRACE_SPAN(name) TRACE_SPAN_MACRO(name, __LINE__)

#endif // KATAGO_TELEMETRY_H
