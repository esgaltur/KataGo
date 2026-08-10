#include "katago_api.h"

#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

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

  const std::string query =
    "{\"id\":\"smoke\",\"rules\":\"chinese\",\"komi\":7.5,"
    "\"boardXSize\":9,\"boardYSize\":9,\"initialPlayer\":\"B\","
    "\"moves\":[],\"maxVisits\":1,\"includeOwnership\":false,"
    "\"includePolicy\":false}";

  bool first = false;
  bool second = false;
  std::thread t1([&]{ first = responseSucceeded(katago_query_json(engine, query.c_str()), "query 1"); });
  std::thread t2([&]{ second = responseSucceeded(katago_query_json(engine, query.c_str()), "query 2"); });
  t1.join();
  t2.join();
  ok = ok && first && second;

  katago_destroy(engine);
  if(!ok)
    return 5;

  std::cout << "KataGo C ABI smoke passed (API " << KATAGO_API_VERSION << ")" << std::endl;
  return 0;
}
