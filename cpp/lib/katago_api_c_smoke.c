#include "katago_api.h"

#include <stddef.h>
#include <string.h>

static void KATAGO_CALL analysis_callback(int query_id, const char* json, const char* error_message, void* user_data) {
  (void)query_id;
  (void)json;
  (void)error_message;
  (void)user_data;
}

static void KATAGO_CALL telemetry_callback(const char* span_name, int64_t duration_us) {
  (void)span_name;
  (void)duration_us;
}

static void KATAGO_CALL telemetry_callback_ex(const char* span_name, int64_t duration_us, void* user_data) {
  (void)span_name;
  (void)duration_us;
  (void)user_data;
}

int main(void) {
  KataGoAnalysisCallback analysis = analysis_callback;
  KataGoTelemetryCallback telemetry = telemetry_callback;
  KataGoTelemetryCallbackEx telemetry_ex = telemetry_callback_ex;
  KataGoCreateOptions options = {0};
  options.structSize = (uint32_t)sizeof(options);
  (void)analysis;
  (void)telemetry;
  (void)telemetry_ex;
  (void)options;

  if(katago_api_version() != KATAGO_API_VERSION)
    return 1;
  if(strcmp(katago_version(), "1.18.2") != 0)
    return 2;
  if(katago_api_version_minor() < 1)
    return 3;
  const uint64_t expected_capabilities =
    KATAGO_CAP_CREATE_OPTIONS |
    KATAGO_CAP_QUERY_CANCELLATION |
    KATAGO_CAP_TELEMETRY_CONTEXT |
    KATAGO_CAP_BOUNDED_ASYNC_QUEUE |
    KATAGO_CAP_STRICT_HISTORY |
    KATAGO_CAP_ADAPTIVE_SEARCH |
    KATAGO_CAP_BUILD_INFO;
  if((katago_api_capabilities() & expected_capabilities) != expected_capabilities)
    return 4;
  const char* build_info = katago_build_info_json();
  if(build_info == NULL || strstr(build_info, "\"upstreamBaseVersion\":\"1.18.2\"") == NULL)
    return 5;
  if(katago_cancel_query_json(NULL, "none") != KATAGO_ERR_INVALID_ARG)
    return 6;
  katago_free_string(NULL);
  if(katago_set_telemetry_callback_ex(NULL, telemetry_ex, NULL) != KATAGO_SUCCESS)
    return 7;
  if(katago_clear_telemetry_callback_and_wait(NULL) != KATAGO_SUCCESS)
    return 8;
  return 0;
}
