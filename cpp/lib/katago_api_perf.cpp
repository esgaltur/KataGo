#include "katago_api.h"
#include "external/nlohmann_json/json.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

void runPerfTest(KataGoEngine* engine, int maxVisits, bool adaptiveSearch, int iterations) {
  std::string query = "{\"id\":\"perf_test\",\"rules\":\"chinese\",\"komi\":7.5,"
                      "\"boardXSize\":19,\"boardYSize\":19,\"initialPlayer\":\"B\","
                      "\"moves\":[],\"maxVisits\":" + std::to_string(maxVisits) + ","
                      "\"includeOwnership\":false,\"includePolicy\":false,"
                      "\"adaptiveSearch\":" + (adaptiveSearch ? "true" : "false") + "}";

  auto start = std::chrono::high_resolution_clock::now();
  
  long long totalFinalVisits = 0;
  for (int i = 0; i < iterations; ++i) {
    const char* response = katago_query_json(engine, query.c_str());
    if (response) {
      if (adaptiveSearch) {
        try {
          nlohmann::json parsed = nlohmann::json::parse(response);
          if (parsed.contains("adaptiveSearchFinalVisits")) {
            totalFinalVisits += parsed["adaptiveSearchFinalVisits"].get<long long>();
          } else {
            totalFinalVisits += maxVisits;
          }
        } catch (...) {
           totalFinalVisits += maxVisits;
        }
      } else {
        totalFinalVisits += maxVisits;
      }
      katago_free_string(response);
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;

  std::cout << "--- Performance Test Result ---" << std::endl;
  std::cout << "Adaptive Search: " << (adaptiveSearch ? "ENABLED" : "DISABLED") << std::endl;
  std::cout << "Initial maxVisits: " << maxVisits << std::endl;
  std::cout << "Iterations: " << iterations << std::endl;
  std::cout << "Total Time: " << elapsed.count() << " ms" << std::endl;
  std::cout << "Average Time per query: " << elapsed.count() / iterations << " ms" << std::endl;
  if (adaptiveSearch) {
      std::cout << "Average final visits: " << (double)totalFinalVisits / iterations << std::endl;
  }
  std::cout << "-------------------------------" << std::endl;
}

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "usage: katago_perf_test <model> <config> [human-model]" << std::endl;
    return 1;
  }

  const char* error = nullptr;
  KataGoEngine* engine = katago_create_ex(
    argv[1], argc == 4 ? argv[3] : nullptr, argv[2], 0, 1, &error
  );

  if (engine == nullptr) {
    std::cerr << "Engine creation failed: " << (error ? error : "unknown error") << std::endl;
    katago_free_string(error);
    return 1;
  }

  std::cout << "Warming up engine..." << std::endl;
  const char* warmup = katago_query_json(engine, "{\"id\":\"warmup\",\"rules\":\"chinese\",\"boardXSize\":9,\"boardYSize\":9,\"maxVisits\":10}");
  if (warmup) katago_free_string(warmup);

  int iterations = 10;
  
  // Baseline: no adaptive search
  runPerfTest(engine, 100, false, iterations);

  // With adaptive search enabled
  runPerfTest(engine, 100, true, iterations);

  katago_destroy(engine);
  return 0;
}
