#include "katago_api.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<int> telemetrySpanCount{0};

void KATAGO_CALL recordTelemetry(const char*, int64_t) {
  telemetrySpanCount.fetch_add(1, std::memory_order_relaxed);
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

  const char* error = nullptr;
  KataGoEngine* engine = katago_create_ex(
    argv[1], argc == 4 ? argv[3] : nullptr, argv[2], 0, 2, &error
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

  const std::string query =
    "{\"id\":\"smoke\",\"rules\":\"chinese\",\"komi\":7.5,"
    "\"boardXSize\":9,\"boardYSize\":9,\"initialPlayer\":\"B\","
    "\"moves\":[],\"maxVisits\":1,\"includeOwnership\":false,"
    "\"includePolicy\":false,\"adaptiveSearch\":true}";

  bool first = false;
  bool second = false;
  katago_set_telemetry_callback(engine, recordTelemetry);
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
  katago_set_telemetry_callback(nullptr, nullptr);
  ok = ok && first && second;
  if(telemetrySpanCount.load(std::memory_order_relaxed) == 0) {
    std::cerr << "telemetry callback did not receive any spans" << std::endl;
    ok = false;
  }

  katago_destroy(engine);
  if(!ok)
    return 5;

  std::cout << "KataGo C ABI smoke passed (API " << KATAGO_API_VERSION << ")" << std::endl;
  return 0;
}
