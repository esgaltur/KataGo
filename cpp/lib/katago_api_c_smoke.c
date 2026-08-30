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

int main(void) {
  KataGoAnalysisCallback analysis = analysis_callback;
  KataGoTelemetryCallback telemetry = telemetry_callback;
  (void)analysis;
  (void)telemetry;

  if(katago_api_version() != KATAGO_API_VERSION)
    return 1;
  if(strcmp(katago_version(), "1.17.2") != 0)
    return 2;
  katago_free_string(NULL);
  katago_set_telemetry_callback(NULL, NULL);
  return 0;
}
