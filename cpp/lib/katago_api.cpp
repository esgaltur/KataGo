/**
 * katago_api.cpp - Thin C adapter layer (Adapter pattern)
 *
 * Translates the flat C API (katago_api.h) into calls on the C++ KataGoEngine
 * and GTPHandler classes. This file contains:
 *   - String ownership helpers (duplicateString / katago_free_string)
 *   - The instance-based API (katago_create / katago_destroy / ...)
 *   - Version:: function definitions (required because the DLL omits main.cpp)
 *
 * No business logic lives here — it all delegates to katago_engine.h and
 * katago_gtp_handler.h.
 */

#include "katago_api.h"
#include "katago_engine.h"
#include "katago_gtp_handler.h"
#include "main.h"

#include <cstring>
#include <new>
#include <sstream>
#include <string>

#include "external/nlohmann_json/json.hpp"

#include "core/using.h"

// ---------------------------------------------------------------------------
// Version functions (normally in main.cpp, but the DLL doesn't include it)
// ---------------------------------------------------------------------------
#ifdef NO_GIT_REVISION
#define GIT_REVISION "<omitted>"
#else
#include <program/gitinfo.h>
#endif

string Version::getKataGoVersion() {
  return string("1.17.2");
}
string Version::getKataGoVersionForHelp() {
  return string("KataGo v1.17.2");
}
string Version::getKataGoVersionFullInfo() {
  ostringstream out;
  out << Version::getKataGoVersionForHelp() << endl;
  out << "Git revision: " << Version::getGitRevision() << endl;
  out << "Compile Time: " << __DATE__ << " " << __TIME__ << endl;
#if defined(USE_CUDA_BACKEND)
  out << "Using CUDA backend" << endl;
#elif defined(USE_TENSORRT_BACKEND)
  out << "Using TensorRT backend" << endl;
#elif defined(USE_OPENCL_BACKEND)
  out << "Using OpenCL backend" << endl;
#elif defined(USE_EIGEN_BACKEND)
  out << "Using Eigen(CPU) backend" << endl;
#else
  out << "Using dummy backend" << endl;
#endif
#if defined(USE_AVX2)
  out << "Compiled with AVX2 and FMA instructions" << endl;
#endif
  return out.str();
}
string Version::getGitRevision() {
  return string(GIT_REVISION);
}
string Version::getGitRevisionWithBackend() {
  string s = string(GIT_REVISION);
#if defined(USE_CUDA_BACKEND)
  s += "-cuda";
#elif defined(USE_TENSORRT_BACKEND)
  s += "-trt";
#elif defined(USE_OPENCL_BACKEND)
  s += "-opencl";
#elif defined(USE_EIGEN_BACKEND)
  s += "-eigen";
#else
  s += "-dummy";
#endif
  return s;
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
namespace {

char* duplicateString(const std::string& s) noexcept {
  char* buf = new(std::nothrow) char[s.size() + 1];
  if(buf == nullptr)
    return nullptr;
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return buf;
}

// Build a well-formed JSON error object, safely escaping the message so that
// quotes, backslashes, and control characters never corrupt the output.
char* makeErrorJson(const std::string& message) noexcept {
  try {
    nlohmann::json j;
    j["error"] = message;
    return duplicateString(j.dump());
  } catch(...) {
    return duplicateString("{\"error\":\"failed to serialize engine error\"}");
  }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Instance-based API
// ---------------------------------------------------------------------------
extern "C" {

KATAGO_API KataGoEngine* KATAGO_CALL katago_create_ex(const char* modelFile,
                                                      const char* humanModelFile,
                                                      const char* configFile,
                                                      int numThreads,
                                                      int numQueryBots,
                                                      const char** outError) {
  if(outError) *outError = nullptr;
  if(!modelFile || !configFile) {
    if(outError) *outError = duplicateString("modelFile and configFile must not be NULL");
    return nullptr;
  }
  if(numThreads < 0 || numQueryBots < 0) {
    if(outError) *outError = duplicateString("numThreads and numQueryBots must not be negative");
    return nullptr;
  }
  try {
    return new KataGoEngine(std::string(modelFile),
                            humanModelFile ? std::string(humanModelFile) : std::string(),
                            std::string(configFile),
                            numThreads,
                            numQueryBots);
  } catch(const std::exception& e) {
    if(outError) *outError = duplicateString(std::string("init failed: ") + e.what());
    return nullptr;
  } catch(...) {
    if(outError) *outError = duplicateString("init failed: unknown native exception");
    return nullptr;
  }
}

KATAGO_API KataGoEngine* KATAGO_CALL katago_create(const char* modelFile,
                                                   const char* configFile,
                                                   int numThreads,
                                                   const char** outError) {
  return katago_create_ex(modelFile, nullptr, configFile, numThreads, 0, outError);
}

KATAGO_API void KATAGO_CALL katago_destroy(KataGoEngine* engine) {
  delete engine;
}

KATAGO_API const char* KATAGO_CALL katago_analyze(KataGoEngine* engine) {
  if(!engine) return nullptr;
  try {
    return duplicateString(engine->analyze());
  } catch(const std::exception& e) {
    return makeErrorJson(e.what());
  } catch(...) {
    return makeErrorJson("unknown native exception");
  }
}

KATAGO_API const char* KATAGO_CALL katago_query_json(KataGoEngine* engine, const char* queryJson) {
  if(!engine || !queryJson) return nullptr;
  try {
    return duplicateString(engine->queryJson(std::string(queryJson)));
  } catch(const std::exception& e) {
    return makeErrorJson(e.what());
  } catch(...) {
    return makeErrorJson("unknown native exception");
  }
}

KATAGO_API const char* KATAGO_CALL katago_gtp_command(KataGoEngine* engine, const char* command) {
  if(!engine || !command) return nullptr;
  try {
    return duplicateString(GTPHandler::dispatch(*engine, std::string(command)));
  } catch(const std::exception& e) {
    return duplicateString(std::string("? ") + e.what());
  } catch(...) {
    return duplicateString("? unknown native exception");
  }
}

KATAGO_API const char* KATAGO_CALL katago_version(void) {
  return "1.17.2";
}

KATAGO_API int KATAGO_CALL katago_api_version(void) {
  return KATAGO_API_VERSION;
}

KATAGO_API int KATAGO_CALL katago_has_human_model(KataGoEngine* engine) {
  if(!engine) return 0;
  return engine->hasHumanModel() ? 1 : 0;
}

KATAGO_API int KATAGO_CALL katago_query_concurrency(KataGoEngine* engine) {
  if(!engine) return 0;
  return engine->queryConcurrency();
}

KATAGO_API int KATAGO_CALL katago_analyze_async(KataGoEngine* engine,
                                                KataGoAnalysisCallback callback,
                                                void* userData,
                                                int* outQueryId) {
  if(!engine || !callback)
    return KATAGO_ERR_INVALID_ARG;
  if(engine->isShuttingDown())
    return KATAGO_ERR_SHUTTING_DOWN;
  try {
    // Wrap the C callback into a C++ lambda that bridges the types.
    auto cppCallback = [callback, userData](int queryId,
                                            const std::string& json,
                                            const std::string& errorMsg) {
      callback(queryId,
               json.empty() ? nullptr : json.c_str(),
               errorMsg.empty() ? nullptr : errorMsg.c_str(),
               userData);
    };
    int qid = engine->submitAnalysisQuery(std::move(cppCallback));
    if(outQueryId) *outQueryId = qid;
    return KATAGO_SUCCESS;
  } catch(...) {
    return engine->isShuttingDown() ? KATAGO_ERR_SHUTTING_DOWN : KATAGO_ERR_ENGINE;
  }
}

KATAGO_API int KATAGO_CALL katago_pending_query_count(KataGoEngine* engine) {
  if(!engine) return 0;
  return engine->pendingQueryCount();
}

KATAGO_API void KATAGO_CALL katago_wait_all_queries(KataGoEngine* engine) {
  if(engine) engine->waitAllQueries();
}

KATAGO_API int KATAGO_CALL katago_wait_all_queries_timeout(KataGoEngine* engine, int timeoutMs) {
  if(!engine) return KATAGO_ERR_INVALID_ARG;
  return engine->waitAllQueries(timeoutMs) ? KATAGO_SUCCESS : KATAGO_ERR_TIMEOUT;
}

KATAGO_API int KATAGO_CALL katago_set_analysis_workers(KataGoEngine* engine, int numWorkers) {
  if(!engine || numWorkers < 1 || numWorkers > 16)
    return KATAGO_ERR_INVALID_ARG;
  return engine->setAnalysisWorkers(numWorkers) ? KATAGO_SUCCESS : KATAGO_ERR_INVALID_STATE;
}

KATAGO_API int KATAGO_CALL katago_board_x_size(KataGoEngine* engine) {
  if(!engine) return 0;
  return engine->boardXSize();
}

KATAGO_API int KATAGO_CALL katago_board_y_size(KataGoEngine* engine) {
  if(!engine) return 0;
  return engine->boardYSize();
}

KATAGO_API const char* KATAGO_CALL katago_show_board(KataGoEngine* engine) {
  if(!engine) return nullptr;
  try {
    return duplicateString(engine->showBoard());
  } catch(...) {
    return nullptr;
  }
}

KATAGO_API void KATAGO_CALL katago_free_string(const char* str) {
  delete[] str;
}

} // extern "C"
