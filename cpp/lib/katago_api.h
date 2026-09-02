/**
 * katago_api.h - C API for KataGo shared library (DLL/.so/.dylib)
 *
 * Design principles:
 *   - Instance-based (opaque handle): supports multiple independent engines.
 *   - Thread-safe: each API call is internally synchronized.
 *   - No global engine handle: instance state lives behind KataGoEngine. The
 *     library initializes KataGo's process-wide immutable lookup tables.
 *   - Clean ownership: caller frees only strings obtained from katago_*() via katago_free_string().
 *   - Multithreading: async analysis via callbacks — submit from any thread,
 *     results delivered on a background worker thread.
 *
 * Usage:
 *   1. Build with -DBUILD_AS_DLL=1.
 *   2. Call katago_create() to obtain an engine handle.
 *   3. Use katago_analyze() / katago_query_json() / katago_gtp_command() for synchronous calls.
 *   4. Use katago_analyze_async() for non-blocking analysis with callbacks.
 *   5. Call katago_destroy() when done (waits for pending async queries).
 */

#ifndef KATAGO_API_H
#define KATAGO_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---------- Export/import & calling convention macros ---------- */
#ifdef _WIN32
  #ifdef KATAGO_DLL_EXPORTS
    #define KATAGO_API __declspec(dllexport)
  #else
    #define KATAGO_API __declspec(dllimport)
  #endif
  #define KATAGO_CALL __cdecl
#else
  #define KATAGO_API __attribute__((visibility("default")))
  #define KATAGO_CALL
#endif

/* ---------- Opaque handle ---------- */
typedef struct KataGoEngine KataGoEngine;

/*
 * Version of this C ABI, independent from the KataGo engine release version.
 * Increment only for an incompatible ABI change; additive symbols keep the
 * same major ABI version.
 */
#define KATAGO_API_VERSION 1
#define KATAGO_API_VERSION_MINOR 1

/* Optional, additive ABI-v1 capabilities returned by katago_api_capabilities(). */
#define KATAGO_CAP_CREATE_OPTIONS       (UINT64_C(1) << 0)
#define KATAGO_CAP_QUERY_CANCELLATION   (UINT64_C(1) << 1)
#define KATAGO_CAP_TELEMETRY_CONTEXT    (UINT64_C(1) << 2)
#define KATAGO_CAP_BOUNDED_ASYNC_QUEUE  (UINT64_C(1) << 3)
#define KATAGO_CAP_STRICT_HISTORY       (UINT64_C(1) << 4)
#define KATAGO_CAP_ADAPTIVE_SEARCH      (UINT64_C(1) << 5)

/* ---------- Error codes ---------- */
enum KataGoError {
  KATAGO_SUCCESS           =  0,
  KATAGO_ERR_INVALID_ARG   = -1,
  KATAGO_ERR_ENGINE        = -2,
  KATAGO_ERR_QUEUE_FULL    = -3,
  KATAGO_ERR_SHUTTING_DOWN = -4,
  KATAGO_ERR_INVALID_STATE = -5,  /* Operation not valid in the current state
                                     (e.g. changing worker count after workers started). */
  KATAGO_ERR_TIMEOUT       = -6,  /* A bounded wait expired before completing. */
  KATAGO_ERR_NOT_FOUND     = -7   /* A requested query was not active. */
};

/** Versioned construction options. Zero-initialize, then set structSize. */
typedef struct KataGoCreateOptions {
  uint32_t structSize;
  int32_t numThreads;
  int32_t numQueryBots;
  int32_t numAsyncWorkers;
  int32_t asyncQueueCapacity;
} KataGoCreateOptions;

/**
 * Callback type for asynchronous analysis results.
 *
 * @param queryId   The ID returned by katago_analyze_async().
 * @param json      JSON result string (valid only for the duration of the callback).
 *                  Copy it if you need it after the callback returns.
 * @param errorMsg  NULL on success, or an error description on failure.
 * @param userData  The opaque pointer passed to katago_analyze_async().
 *
 * NOTE: The callback is invoked on a background worker thread.
 *       Keep it short or dispatch work elsewhere to avoid blocking the queue.
 */
typedef void (KATAGO_CALL *KataGoAnalysisCallback)(int queryId,
                                                   const char* json,
                                                   const char* errorMsg,
                                                   void* userData);

/* ---------- Lifecycle ---------- */

/**
 * Create and initialize a KataGo engine instance.
 *
 * @param modelFile   Path to the neural network model file (.bin.gz)
 * @param configFile  Path to the GTP/analysis config file
 * @param numThreads  Number of search threads (0 = use config default)
 * @param outError    If non-NULL and creation fails, receives a heap-allocated
 *                    error message (free with katago_free_string).
 * @return An opaque engine handle, or NULL on failure.
 */
KATAGO_API KataGoEngine* KATAGO_CALL katago_create(const char* modelFile,
                                                   const char* configFile,
                                                   int numThreads,
                                                   const char** outError);

/**
 * Create an engine with the full set of options.
 *
 * katago_create() is this function with humanModelFile = NULL and
 * numQueryBots = 0.
 *
 * @param modelFile       Path to the neural network model file (.bin.gz)
 * @param humanModelFile  Path to a human SL model (.bin.gz), or NULL for none.
 *                        Without one, queries whose overrideSettings carry a
 *                        humanSLProfile are rejected rather than silently
 *                        answered by the ordinary model.
 * @param configFile      Path to the GTP/analysis config file
 * @param numThreads      Search threads per query (0 = use config default)
 * @param numQueryBots    How many katago_query_json() calls may run at once
 *                        (0 = numAnalysisThreads from the config, else 1;
 *                        capped at 64). Each one costs a search tree, so this
 *                        trades memory for throughput.
 * @param outError        If non-NULL and creation fails, receives a
 *                        heap-allocated message (free with katago_free_string).
 * @return An opaque engine handle, or NULL on failure.
 */
KATAGO_API KataGoEngine* KATAGO_CALL katago_create_ex(const char* modelFile,
                                                      const char* humanModelFile,
                                                      const char* configFile,
                                                      int numThreads,
                                                      int numQueryBots,
                                                      const char** outError);

/**
 * Additive constructor with explicit async worker and queue sizing.
 * Zero values select the same defaults as katago_create_ex(). `structSize`
 * must be at least sizeof(KataGoCreateOptions); larger future structs are
 * accepted and their unknown suffix is ignored.
 */
KATAGO_API KataGoEngine* KATAGO_CALL katago_create_with_options(
  const char* modelFile,
  const char* humanModelFile,
  const char* configFile,
  const KataGoCreateOptions* options,
  const char** outError
);

/**
 * Destroy an engine instance and release all resources.
 * Passing NULL is a safe no-op.
 *
 * The caller must first prevent new calls and wait for synchronous calls on
 * this handle to return. Destruction itself drains the async worker queue.
 */
KATAGO_API void KATAGO_CALL katago_destroy(KataGoEngine* engine);

/* ---------- Analysis (synchronous) ---------- */

/**
 * Run analysis on the engine's current board position (blocking).
 *
 * Returns a comprehensive JSON string with rootInfo (winrate, scoreLead, visits, utility),
 * moveInfos (candidate moves with policy prior, pv sequence, scoreLead, winrate),
 * and ownership map grid.
 * The caller must free the result with katago_free_string().
 * Thread-safe: multiple threads may call this, but calls are serialized internally.
 *
 * @return JSON result string, or NULL on error.
 */
KATAGO_API const char* KATAGO_CALL katago_analyze(KataGoEngine* engine);

/**
 * Submit a raw JSON analysis query string against the engine.
 *
 * Supported query fields:
 *   id, rules, komi, boardXSize, boardYSize, initialPlayer, initialStones,
 *   moves, maxVisits, analysisPVLen, includeOwnership, includePolicy,
 *   adaptiveSearch, adaptiveVisitRatio, adaptiveUtilityTolerance,
 *   adaptiveMaxMultiplier, adaptiveStepMultiplier, strictHistory,
 *   whiteHandicapBonus, overrideSettings.
 *
 * `overrideSettings` accepts any config key the analysis engine accepts —
 * including humanSLProfile and the chosenMove* knobs — and is applied to a copy
 * of the engine's config, so settings a query does not mention keep their
 * configured value. An unknown key is an error, not a silent no-op.
 *
 * The query is stateless: the position comes entirely from the request and the
 * engine's own board is neither read nor written. Up to numQueryBots calls run
 * concurrently; further callers queue.
 *
 * Winrates are reported from the config's reportAnalysisWinratesAs perspective
 * (default BLACK), not from the side to move.
 *
 * Returns the analysis JSON response, or an object of the form
 * `{"id":…,"error":…}` when the query is rejected. Either way the caller frees
 * it with katago_free_string(). Returns NULL only if engine or queryJson is NULL.
 */
KATAGO_API const char* KATAGO_CALL katago_query_json(KataGoEngine* engine, const char* queryJson);

/**
 * Cooperatively stop an active JSON query identified by its request `id`.
 * IDs should be unique among concurrent calls. Returns KATAGO_SUCCESS when a
 * matching active search was signaled, KATAGO_ERR_NOT_FOUND otherwise, or
 * KATAGO_ERR_INVALID_ARG for NULL/empty input. The query call still owns its
 * response and must be joined by the host.
 */
KATAGO_API int KATAGO_CALL katago_cancel_query_json(KataGoEngine* engine, const char* queryId);

/* ---------- Analysis (asynchronous) ---------- */

/**
 * Submit an asynchronous analysis query (non-blocking).
 *
 * Takes a snapshot of the current board position and enqueues it for analysis
 * on a persistent background worker thread. The result is delivered via the callback.
 * Safe to call from any thread, including concurrently.
 *
 * @param engine    Engine handle.
 * @param callback  Function to call when analysis completes (on worker thread).
 * @param userData  Opaque pointer forwarded to the callback.
 * @param outQueryId  If non-NULL, receives the query ID for tracking.
 * @return KATAGO_SUCCESS, KATAGO_ERR_INVALID_ARG if engine/callback is NULL,
 *         KATAGO_ERR_QUEUE_FULL when the configured bound is reached,
 *         KATAGO_ERR_SHUTTING_DOWN if the engine is being destroyed,
 *         or KATAGO_ERR_ENGINE on internal failure.
 */
KATAGO_API int KATAGO_CALL katago_analyze_async(KataGoEngine* engine,
                                                KataGoAnalysisCallback callback,
                                                void* userData,
                                                int* outQueryId);

/**
 * Get the number of pending (not yet completed) async queries.
 * Thread-safe.
 */
KATAGO_API int KATAGO_CALL katago_pending_query_count(KataGoEngine* engine);

/**
 * Wait until all pending async queries have completed.
 * Blocks the calling thread. Thread-safe.
 */
KATAGO_API void KATAGO_CALL katago_wait_all_queries(KataGoEngine* engine);

/**
 * Wait until all pending async queries have completed, or until the timeout elapses.
 * Blocks the calling thread. Thread-safe.
 *
 * @param engine     Engine handle.
 * @param timeoutMs  Maximum time to wait in milliseconds. A negative value waits forever.
 * @return KATAGO_SUCCESS if all queries completed, KATAGO_ERR_TIMEOUT on timeout,
 *         or KATAGO_ERR_INVALID_ARG if engine is NULL.
 */
KATAGO_API int KATAGO_CALL katago_wait_all_queries_timeout(KataGoEngine* engine, int timeoutMs);

/* ---------- Telemetry ---------- */

/**
 * Callback type for FFI telemetry and tracing.
 * Called by the engine when a significant internal span completes.
 * @param spanName  The name of the measured operation (e.g., "query_json", "search").
 * @param durationUs The duration of the operation in microseconds.
 */
typedef void (KATAGO_CALL *KataGoTelemetryCallback)(const char* spanName, int64_t durationUs);

typedef void (KATAGO_CALL *KataGoTelemetryCallbackEx)(const char* spanName,
                                                      int64_t durationUs,
                                                      void* userData);

/**
 * Set the telemetry callback to bridge KataGo internal timings to the host's tracing system.
 *
 * The callback is process-wide (the engine parameter is retained for API
 * consistency and may be NULL), may run concurrently on multiple engine/search
 * threads, and remains installed until replaced or cleared by passing NULL.
 * Changing it does not wait for a callback that is already in progress.
 */
KATAGO_API void KATAGO_CALL katago_set_telemetry_callback(KataGoEngine* engine, KataGoTelemetryCallback callback);

/**
 * Install a callback with an opaque host context. The context is borrowed and
 * may be read concurrently until the registration is quiescent.
 */
KATAGO_API int KATAGO_CALL katago_set_telemetry_callback_ex(KataGoEngine* engine,
                                                            KataGoTelemetryCallbackEx callback,
                                                            void* userData);

/**
 * Clear telemetry and wait for callbacks already in progress before freeing
 * userData. Calling clear from inside a telemetry callback returns
 * KATAGO_ERR_INVALID_STATE instead of deadlocking.
 */
KATAGO_API int KATAGO_CALL katago_clear_telemetry_callback_and_wait(KataGoEngine* engine);

/* ---------- Board introspection ---------- */

/**
 * Get the current board width (X size).
 * @return The board X size, or 0 if engine is NULL. Thread-safe.
 */
KATAGO_API int KATAGO_CALL katago_board_x_size(KataGoEngine* engine);

/**
 * Get the current board height (Y size).
 * @return The board Y size, or 0 if engine is NULL. Thread-safe.
 */
KATAGO_API int KATAGO_CALL katago_board_y_size(KataGoEngine* engine);

/**
 * Get a human-readable rendering of the current board position.
 * The caller must free the result with katago_free_string().
 * @return A newly allocated board string, or NULL if engine is NULL. Thread-safe.
 */
KATAGO_API const char* KATAGO_CALL katago_show_board(KataGoEngine* engine);

/* ---------- GTP ---------- */

/**
 * Execute a single GTP command (e.g. "boardsize 19", "play B D4", "genmove W", "kata-analyze").
 *
 * Supported commands: boardsize, clear_board, komi, play, genmove, undo,
 *                     showboard, name, version, protocol_version, list_commands,
 *                     known_command, kata-set-rules, kata-analyze, quit.
 *
 * @return GTP response string (free with katago_free_string()), or NULL on error.
 */
KATAGO_API const char* KATAGO_CALL katago_gtp_command(KataGoEngine* engine, const char* command);

/* ---------- Utilities ---------- */

/** Get the KataGo version string (static — do NOT free). Thread-safe. */
KATAGO_API const char* KATAGO_CALL katago_version(void);

/** Get KATAGO_API_VERSION for runtime compatibility checks. Thread-safe. */
KATAGO_API int KATAGO_CALL katago_api_version(void);

/** Additive revision within KATAGO_API_VERSION. */
KATAGO_API int KATAGO_CALL katago_api_version_minor(void);

/** Bitset of KATAGO_CAP_* extensions supported by this library. */
KATAGO_API uint64_t KATAGO_CALL katago_api_capabilities(void);

/**
 * Whether a human SL model was loaded (see katago_create_ex).
 * @return 1 if present, 0 if not or if engine is NULL. Thread-safe.
 */
KATAGO_API int KATAGO_CALL katago_has_human_model(KataGoEngine* engine);

/**
 * How many katago_query_json() calls this engine runs concurrently.
 * @return The pool size, or 0 if engine is NULL. Thread-safe.
 */
KATAGO_API int KATAGO_CALL katago_query_concurrency(KataGoEngine* engine);

/** Free a heap-allocated string returned by any katago_*() function. */
KATAGO_API void KATAGO_CALL katago_free_string(const char* str);

/* ---------- Thread pool configuration ---------- */

/**
 * Set the number of background worker threads for async analysis.
 * Default is 1. Must be called before the first katago_analyze_async().
 *
 * @param engine       Engine handle.
 * @param numWorkers   Number of worker threads (1..16).
 * @return KATAGO_SUCCESS, KATAGO_ERR_INVALID_ARG if engine is NULL or numWorkers is
 *         out of range, or KATAGO_ERR_INVALID_STATE if workers have already started.
 */
KATAGO_API int KATAGO_CALL katago_set_analysis_workers(KataGoEngine* engine, int numWorkers);

#ifdef __cplusplus
}
#endif

#endif /* KATAGO_API_H */
