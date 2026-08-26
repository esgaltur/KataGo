#ifndef KATAGO_TELEMETRY_H
#define KATAGO_TELEMETRY_H

#include <chrono>
#include "katago_api.h"

#ifdef __cplusplus
extern "C" {
#endif
extern KataGoTelemetryCallback g_telemetry_callback;
#ifdef __cplusplus
}
#endif

struct TraceSpan {
  const char* name;
  std::chrono::steady_clock::time_point start;
  
  TraceSpan(const char* n) : name(n), start(std::chrono::steady_clock::now()) {}
  
  ~TraceSpan() {
    if(g_telemetry_callback) {
      auto end = std::chrono::steady_clock::now();
      int64_t dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
      g_telemetry_callback(name, dur);
    }
  }
};

#define TRACE_SPAN_CONCAT(a, b) a ## b
#define TRACE_SPAN_MACRO(name, line) TraceSpan TRACE_SPAN_CONCAT(__span_, line)(name)
#define TRACE_SPAN(name) TRACE_SPAN_MACRO(name, __LINE__)

#endif // KATAGO_TELEMETRY_H
