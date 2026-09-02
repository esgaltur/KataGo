#include "telemetry.h"

#include <condition_variable>
#include <mutex>

namespace {

  std::mutex telemetryMutex;
  std::condition_variable telemetryCV;
  KataGoTelemetryCallback telemetryCallback = nullptr;
  KataGoTelemetryCallbackEx telemetryCallbackEx = nullptr;
  void* telemetryUserData = nullptr;
  size_t telemetryInFlight = 0;
  thread_local size_t telemetryCallbackDepth = 0;

}

namespace KataGoTelemetry {

  void setCallback(KataGoTelemetryCallback callback) noexcept {
    std::lock_guard<std::mutex> lock(telemetryMutex);
    telemetryCallback = callback;
    telemetryCallbackEx = nullptr;
    telemetryUserData = nullptr;
  }

  void setCallbackEx(KataGoTelemetryCallbackEx callback, void* userData) noexcept {
    std::lock_guard<std::mutex> lock(telemetryMutex);
    telemetryCallback = nullptr;
    telemetryCallbackEx = callback;
    telemetryUserData = callback == nullptr ? nullptr : userData;
  }

  bool clearCallbackAndWait() noexcept {
    if(telemetryCallbackDepth > 0)
      return false;
    std::unique_lock<std::mutex> lock(telemetryMutex);
    telemetryCallback = nullptr;
    telemetryCallbackEx = nullptr;
    telemetryUserData = nullptr;
    telemetryCV.wait(lock, []{ return telemetryInFlight == 0; });
    return true;
  }

  void emit(const char* spanName, int64_t durationUs) noexcept {
    KataGoTelemetryCallback callback;
    KataGoTelemetryCallbackEx callbackEx;
    void* userData;
    {
      std::lock_guard<std::mutex> lock(telemetryMutex);
      callback = telemetryCallback;
      callbackEx = telemetryCallbackEx;
      userData = telemetryUserData;
      if(callback == nullptr && callbackEx == nullptr)
        return;
      telemetryInFlight++;
    }

    telemetryCallbackDepth++;
    try {
      if(callbackEx != nullptr)
        callbackEx(spanName, durationUs, userData);
      else
        callback(spanName, durationUs);
    } catch(...) {}
    telemetryCallbackDepth--;

    {
      std::lock_guard<std::mutex> lock(telemetryMutex);
      telemetryInFlight--;
      if(telemetryInFlight == 0)
        telemetryCV.notify_all();
    }
  }

}  // namespace KataGoTelemetry
