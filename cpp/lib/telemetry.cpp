#include "telemetry.h"

#include <atomic>

namespace {

  std::atomic<KataGoTelemetryCallback> telemetryCallback{nullptr};

}

namespace KataGoTelemetry {

  void setCallback(KataGoTelemetryCallback callback) noexcept {
    telemetryCallback.store(callback, std::memory_order_release);
  }

  KataGoTelemetryCallback getCallback() noexcept {
    return telemetryCallback.load(std::memory_order_acquire);
  }

}  // namespace KataGoTelemetry
