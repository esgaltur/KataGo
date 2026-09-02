#include "katago_api.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<int> telemetrySpanCount{0};

void KATAGO_CALL recordTelemetry(const char*, int64_t, void* userData) {
  static_cast<std::atomic<int>*>(userData)->fetch_add(1, std::memory_order_relaxed);
}

struct BlockingCallbackState {
  std::atomic<int> entered{0};
  std::atomic<bool> release{false};
};

void KATAGO_CALL blockingCallback(int, const char*, const char*, void* userData) {
  BlockingCallbackState* state = static_cast<BlockingCallbackState*>(userData);
  state->entered.fetch_add(1, std::memory_order_release);
  while(!state->release.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

bool responseSucceeded(const char* response, const char* label) {
  if(response == nullptr) {
    std::cerr << label << " returned NULL" << std::endl;
    return false;
  }
  const bool ok = std::strstr(response, "\"error\"") == nullptr;
  if(!ok)
    std::cerr << label << " failed: " << response << std::endl;
  katago_free_string(response);
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  if(argc < 3 || argc > 4) {
    std::cerr << "usage: katago_dll_smoke <model> <config> [human-model]" << std::endl;
    return 2;
  }
  if(katago_api_version() != KATAGO_API_VERSION) {
    std::cerr << "C ABI version mismatch" << std::endl;
    return 3;
  }
  const uint64_t expectedCapabilities =
    KATAGO_CAP_CREATE_OPTIONS |
    KATAGO_CAP_QUERY_CANCELLATION |
    KATAGO_CAP_TELEMETRY_CONTEXT |
    KATAGO_CAP_BOUNDED_ASYNC_QUEUE |
    KATAGO_CAP_STRICT_HISTORY |
    KATAGO_CAP_ADAPTIVE_SEARCH;
  if(katago_api_version_minor() < 1 ||
     (katago_api_capabilities() & expectedCapabilities) != expectedCapabilities) {
    std::cerr << "C ABI capabilities are incomplete" << std::endl;
    return 3;
  }

  const char* error = nullptr;
  KataGoCreateOptions createOptions{};
  createOptions.structSize = sizeof(createOptions);
  createOptions.numQueryBots = 2;
  createOptions.numAsyncWorkers = 1;
  createOptions.asyncQueueCapacity = 1;
  KataGoEngine* engine = katago_create_with_options(
    argv[1], argc == 4 ? argv[3] : nullptr, argv[2], &createOptions, &error
  );
  if(engine == nullptr) {
    std::cerr << "create failed: " << (error == nullptr ? "unknown" : error) << std::endl;
    katago_free_string(error);
    return 4;
  }

  bool ok = katago_query_concurrency(engine) == 2;
  if(!ok)
    std::cerr << "query concurrency did not match requested value" << std::endl;

  const char* missingBoardSize = katago_gtp_command(engine, "boardsize");
  if(missingBoardSize == nullptr || missingBoardSize[0] != '?') {
    std::cerr << "missing boardsize argument did not produce a GTP error" << std::endl;
    ok = false;
  }
  katago_free_string(missingBoardSize);

  const char* missingKomi = katago_gtp_command(engine, "komi");
  if(missingKomi == nullptr || missingKomi[0] != '?') {
    std::cerr << "missing komi argument did not produce a GTP error" << std::endl;
    ok = false;
  }
  katago_free_string(missingKomi);

  const char* emptySuccess = katago_gtp_command(engine, "clear_board");
  if(emptySuccess == nullptr || std::strcmp(emptySuccess, "=\n\n") != 0) {
    std::cerr << "empty GTP success was not framed correctly: "
              << (emptySuccess == nullptr ? "NULL" : emptySuccess) << std::endl;
    ok = false;
  }
  katago_free_string(emptySuccess);

  const char* numberedGtp = katago_gtp_command(engine, "42 protocol_version");
  if(numberedGtp == nullptr || std::strcmp(numberedGtp, "=42 2\n\n") != 0) {
    std::cerr << "numbered GTP response was not framed correctly: "
              << (numberedGtp == nullptr ? "NULL" : numberedGtp) << std::endl;
    ok = false;
  }
  katago_free_string(numberedGtp);

  const char* malformed = katago_query_json(engine, "{");
  if(malformed == nullptr || std::strstr(malformed, "\"error\"") == nullptr) {
    std::cerr << "malformed JSON did not produce a structured error" << std::endl;
    ok = false;
  }
  katago_free_string(malformed);

  const char* illegal = katago_query_json(
    engine,
    "{\"id\":\"illegal\",\"rules\":\"chinese\",\"boardXSize\":9,"
    "\"boardYSize\":9,\"initialPlayer\":\"B\","
    "\"moves\":[[\"B\",\"D4\"],[\"W\",\"D4\"]],\"maxVisits\":1}"
  );
  if(illegal == nullptr || std::strstr(illegal, "\"error\"") == nullptr) {
    std::cerr << "illegal move history did not produce a structured error" << std::endl;
    ok = false;
  }
  katago_free_string(illegal);

  const char* invalidAdaptive = katago_query_json(
    engine,
    "{\"id\":\"bad-adaptive\",\"adaptiveVisitRatio\":1.25}"
  );
  if(invalidAdaptive == nullptr || std::strstr(invalidAdaptive, "\"error\"") == nullptr) {
    std::cerr << "invalid adaptive settings did not produce a structured error" << std::endl;
    ok = false;
  }
  katago_free_string(invalidAdaptive);

  const char* invalidRules = katago_query_json(
    engine,
    "{\"id\":\"bad-rules\",\"rules\":7,\"maxVisits\":2}"
  );
  if(invalidRules == nullptr || std::strstr(invalidRules, "\"error\"") == nullptr) {
    std::cerr << "invalid rules type did not produce a structured error" << std::endl;
    ok = false;
  }
  katago_free_string(invalidRules);

  const char* invalidKomi = katago_query_json(
    engine,
    "{\"id\":\"bad-komi\",\"komi\":7.25,\"maxVisits\":2}"
  );
  if(invalidKomi == nullptr || std::strstr(invalidKomi, "\"error\"") == nullptr) {
    std::cerr << "quarter-point komi did not produce a structured error" << std::endl;
    ok = false;
  }
  katago_free_string(invalidKomi);

  const char* wrongPlayer = katago_query_json(
    engine,
    "{\"id\":\"wrong-player\",\"rules\":\"chinese\",\"boardXSize\":9,"
    "\"boardYSize\":9,\"initialPlayer\":\"B\",\"strictHistory\":true,"
    "\"moves\":[[\"W\",\"D4\"]],\"maxVisits\":2}"
  );
  if(wrongPlayer == nullptr || std::strstr(wrongPlayer, "wrong player") == nullptr) {
    std::cerr << "strict history accepted the wrong player" << std::endl;
    ok = false;
  }
  katago_free_string(wrongPlayer);

  const std::string query =
    "{\"id\":\"smoke\",\"rules\":\"chinese\",\"komi\":7.5,"
    "\"boardXSize\":9,\"boardYSize\":9,\"initialPlayer\":\"B\","
    "\"moves\":[],\"maxVisits\":8,\"includeOwnership\":false,"
    "\"includePolicy\":false,\"adaptiveSearch\":true,"
    "\"adaptiveVisitRatio\":0.0,\"adaptiveUtilityTolerance\":100.0,"
    "\"adaptiveMaxMultiplier\":2.0,\"adaptiveStepMultiplier\":0.5}";

  bool first = false;
  bool second = false;
  if(katago_set_telemetry_callback_ex(engine, recordTelemetry, &telemetrySpanCount) != KATAGO_SUCCESS) {
    std::cerr << "could not install telemetry callback with context" << std::endl;
    ok = false;
  }
  std::thread t1([&]{
    const char* response = katago_query_json(engine, query.c_str());
    first = responseSucceeded(response, "query 1");
  });
  std::thread t2([&]{
    const char* response = katago_query_json(engine, query.c_str());
    second = responseSucceeded(response, "query 2");
  });
  t1.join();
  t2.join();
  if(katago_clear_telemetry_callback_and_wait(engine) != KATAGO_SUCCESS) {
    std::cerr << "could not quiesce telemetry callback" << std::endl;
    ok = false;
  }
  ok = ok && first && second;
  if(telemetrySpanCount.load(std::memory_order_relaxed) == 0) {
    std::cerr << "telemetry callback did not receive any spans" << std::endl;
    ok = false;
  }
  const int telemetryCountAfterClear = telemetrySpanCount.load(std::memory_order_relaxed);

  const char* adaptive = katago_query_json(engine, query.c_str());
  if(adaptive == nullptr || std::strstr(adaptive, "\"adaptiveSearch\":true") == nullptr ||
     std::strstr(adaptive, "\"adaptiveSearchInitialVisits\":8") == nullptr ||
     std::strstr(adaptive, "\"adaptiveSearchFinalVisits\"") == nullptr) {
    std::cerr << "adaptive search did not extend and report its visit budget: "
              << (adaptive == nullptr ? "NULL" : adaptive) << std::endl;
    ok = false;
  }
  katago_free_string(adaptive);
  if(telemetrySpanCount.load(std::memory_order_relaxed) != telemetryCountAfterClear) {
    std::cerr << "telemetry callback ran after quiescent clear returned" << std::endl;
    ok = false;
  }

  // Hold the sole worker inside its callback. This deterministically leaves
  // exactly one queued item and proves the configured queue bound is enforced.
  const char* boardSizeResponse = katago_gtp_command(engine, "boardsize 9");
  katago_free_string(boardSizeResponse);
  BlockingCallbackState callbackState;
  int firstAsyncId = 0;
  if(katago_analyze_async(engine, blockingCallback, &callbackState, &firstAsyncId) != KATAGO_SUCCESS) {
    std::cerr << "first async query was rejected" << std::endl;
    ok = false;
  }
  const auto callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while(callbackState.entered.load(std::memory_order_acquire) == 0 &&
        std::chrono::steady_clock::now() < callbackDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if(callbackState.entered.load(std::memory_order_acquire) == 0) {
    std::cerr << "async callback did not start" << std::endl;
    ok = false;
  }
  int secondAsyncId = 0;
  int thirdAsyncId = 0;
  const int secondAsyncResult = katago_analyze_async(
    engine, blockingCallback, &callbackState, &secondAsyncId
  );
  const int thirdAsyncResult = katago_analyze_async(
    engine, blockingCallback, &callbackState, &thirdAsyncId
  );
  if(secondAsyncResult != KATAGO_SUCCESS || thirdAsyncResult != KATAGO_ERR_QUEUE_FULL) {
    std::cerr << "bounded async queue returned " << secondAsyncResult << " and "
              << thirdAsyncResult << std::endl;
    ok = false;
  }
  if(katago_wait_all_queries_timeout(engine, 1) != KATAGO_ERR_TIMEOUT) {
    std::cerr << "timed async wait did not report a timeout" << std::endl;
    ok = false;
  }
  callbackState.release.store(true, std::memory_order_release);
  katago_wait_all_queries(engine);
  if(katago_pending_query_count(engine) != 0 ||
     callbackState.entered.load(std::memory_order_acquire) != 2) {
    std::cerr << "async query accounting did not drain cleanly" << std::endl;
    ok = false;
  }

  // The query ID is registered while a synchronous search owns a pool bot.
  // Cancellation must find it, stop the search, and still return valid JSON.
  bool cancelledQuerySucceeded = false;
  std::thread cancellable([&]{
    const char* response = katago_query_json(
      engine,
      "{\"id\":\"cancel-me\",\"rules\":\"chinese\",\"boardXSize\":9,"
      "\"boardYSize\":9,\"moves\":[],\"maxVisits\":100000}"
    );
    cancelledQuerySucceeded = responseSucceeded(response, "cancelled query");
  });
  bool cancellationFound = false;
  const auto cancellationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while(std::chrono::steady_clock::now() < cancellationDeadline) {
    const int result = katago_cancel_query_json(engine, "cancel-me");
    if(result == KATAGO_SUCCESS) {
      cancellationFound = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  cancellable.join();
  if(!cancellationFound || !cancelledQuerySucceeded) {
    std::cerr << "active synchronous query cancellation failed" << std::endl;
    ok = false;
  }

  katago_destroy(engine);
  if(!ok)
    return 5;

  std::cout << "KataGo C ABI smoke passed (API " << KATAGO_API_VERSION << ")" << std::endl;
  return 0;
}
