# KataGo Shared Library (C API)

This folder contains the source for building KataGo as a **shared library**
(`katago.dll` on Windows, `libkatago.so` on Linux, `libkatago.dylib` on macOS)
with a stable, flat **C API**. The C API lets any language with C FFI
(C, C++, C#, Python/ctypes, Rust, Go, Java/JNA, etc.) embed a full KataGo
analysis/GTP engine in-process without shelling out to the executable.

## Contents

| File | Layer | Responsibility |
|------|-------|----------------|
| `katago_api.h`           | Public C API | Opaque handle, error codes, all `katago_*` functions. **This is the only header consumers need.** |
| `katago_api.cpp`         | Adapter      | Thin `extern "C"` layer translating the C API into C++ calls. Also defines `Version::` symbols (the DLL omits `main.cpp`). |
| `katago_engine.h/.cpp`   | Facade       | `KataGoEngine` — owns the `Logger`, `NNEvaluator`, `AsyncBot`, board/game state, and the async worker pool. Internal only. |
| `katago_gtp_handler.h/.cpp` | Dispatcher | Parses a GTP command line and routes it to the engine via a command registry. Internal only. |

Architecture: **Adapter (`katago_api.cpp`) → Facade (`KataGoEngine`) → Command dispatcher (`GTPHandler`)**.
Only `katago_api.h` is part of the public contract; the other headers are internal.

## Design principles

- **Instance-based / opaque handle** — `katago_create()` returns a `KataGoEngine*`;
  multiple independent engines can coexist. KataGo's immutable lookup tables are
  process-wide and initialized by the library; mutable engine state is per handle.
- **Thread-safe while the handle is live** — stateful calls are serialized per
  engine; raw JSON queries use a bounded per-engine bot pool.
- **Clean ownership** — every `const char*` returned by a `katago_*()` function
  (except `katago_version()`) is heap-allocated and **must** be freed with
  `katago_free_string()`.
- **Async analysis** — `katago_analyze_async()` snapshots the current position and
  runs it on a background worker pool, delivering the result via a callback.
- **Additive, ABI-stable evolution** — new functions and error codes are appended;
  existing signatures do not change.

---

## Building

Use the provided Windows batch script from the `cpp/` directory:

```bat
build_dll.bat            REM EIGEN (CPU) backend — default
build_dll.bat CUDA       REM NVIDIA CUDA backend
build_dll.bat OPENCL     REM OpenCL backend
build_dll.bat EIGEN      REM Eigen CPU backend
```

Prerequisites: Visual Studio 2019/2022 Build Tools, CMake, and (recommended) vcpkg
for `zlib` / `eigen3`. Outputs land in `cpp/build_dll/`:

- `katago.dll` — the shared library
- `katago.lib` — MSVC import library
- `katago_dll_smoke.exe` — public-ABI verification harness

To configure manually with CMake, set `-DBUILD_AS_DLL=1` alongside a backend, e.g.:

```bash
cmake -B build_dll -G Ninja -DBUILD_AS_DLL=1 -DUSE_BACKEND=EIGEN -DUSE_AVX2=1 -DNO_GIT_REVISION=1
cmake --build build_dll
```

### Linux and WSL

The Linux helper defaults to OpenCL and also accepts `EIGEN`, `CUDA`, or
`TENSORRT`:

```bash
# Inside Linux
bash cpp/build_shared_linux.sh OPENCL

# From Windows using WSL
wsl -d Ubuntu -- bash /mnt/c/path/to/KataGo/cpp/build_shared_linux.sh OPENCL
```

Typical OpenCL build prerequisites on Ubuntu are CMake, a C++ compiler, zlib
headers, OpenCL headers, and an OpenCL loader. Compilation does not require the
build environment to expose a GPU. The real smoke test does: run it in the
target Docker/native Linux environment with the vendor ICD and GPU device
available.

For a dependency prefix unpacked without system installation, set
`KATAGO_DEP_PREFIX` to its `usr` directory. For example:

```bash
KATAGO_DEP_PREFIX=/path/to/prefix/usr \
  bash cpp/build_shared_linux.sh OPENCL
```

The helper fails unless the resulting ELF library exports exactly the 19
documented `katago_*` C functions and no C++ implementation symbols.

### Linking against the library

1. Copy `katago.dll` next to your executable (and any backend runtime DLLs, e.g. `zlib1.dll`).
2. Link against `katago.lib` (MSVC) or `katago.dll` (MinGW).
3. `#include "katago_api.h"` (add this `lib/` folder to your include path).

Run the harness with real assets after building:

```bat
build_dll\katago_dll_smoke.exe model.bin.gz analysis.cfg human_model.bin.gz
```

The harness checks ABI versioning, structured malformed/illegal-input errors,
two concurrent searches, string ownership, and clean destruction.

---

## API reference

All functions use the `KATAGO_CALL` (`__cdecl` on Windows) calling convention and are
exported with `KATAGO_API`.

### Error codes (`enum KataGoError`)

| Code | Value | Meaning |
|------|-------|---------|
| `KATAGO_SUCCESS`           | `0`  | Operation succeeded. |
| `KATAGO_ERR_INVALID_ARG`   | `-1` | A NULL handle or out-of-range/invalid argument. |
| `KATAGO_ERR_ENGINE`        | `-2` | Internal engine failure. |
| `KATAGO_ERR_QUEUE_FULL`    | `-3` | Async queue is full (reserved). |
| `KATAGO_ERR_SHUTTING_DOWN` | `-4` | The engine is being destroyed; async submission rejected. |
| `KATAGO_ERR_INVALID_STATE` | `-5` | Operation invalid in the current state (e.g. changing worker count after workers started). |
| `KATAGO_ERR_TIMEOUT`       | `-6` | A bounded wait expired before completing. |

### Lifecycle

| Function | Description |
|----------|-------------|
| `int katago_api_version(void)` | C ABI version (`KATAGO_API_VERSION`), independent from the KataGo engine version. |
| `KataGoEngine* katago_create_ex(const char* modelFile, const char* humanModelFile, const char* configFile, int numThreads, int numQueryBots, const char** outError)` | Create and initialize an engine. `humanModelFile` may be `NULL`. `numThreads` of `0` uses the config's `numSearchThreads`; `numQueryBots` of `0` uses `numAnalysisThreads` (capped at 64). On failure returns `NULL` and, if `outError` is non-NULL, sets a heap-allocated message (free with `katago_free_string`). |
| `KataGoEngine* katago_create(const char* modelFile, const char* configFile, int numThreads, const char** outError)` | Shorthand for `katago_create_ex` with no human model and config-derived concurrency. |
| `void katago_destroy(KataGoEngine* engine)` | Destroy an engine and release all resources. Waits for in-flight async work. `NULL` is a safe no-op. |

Without a `humanModelFile`, a query whose `overrideSettings` carry a
`humanSLProfile` is **rejected** rather than quietly answered by the ordinary
model. `katago_has_human_model()` reports which you have.

`numQueryBots` is the number of `katago_query_json` calls that run at once.
Each costs a search tree, so it trades memory for throughput; the default keeps
one config file valid for both this library and `katago.exe analysis`.

### Analysis — synchronous

| Function | Returns / ownership |
|----------|---------------------|
| `const char* katago_analyze(KataGoEngine* engine)` | Analyzes the engine's current position. Returns a JSON string with `rootInfo`, `moveInfos`, and `ownership`. **Free with `katago_free_string`.** `NULL` if `engine` is NULL. |
| `const char* katago_query_json(KataGoEngine* engine, const char* queryJson)` | Runs a raw KataGo analysis query (see the field table below). Returns the full analysis JSON. **Free with `katago_free_string`.** |

On internal error these return a valid JSON error object — `{"id":"…","error":"…"}`
when the request had a parseable `id`, so a caller can fail the one query
responsible. The message is properly JSON-escaped.

#### `katago_query_json` fields

| Field | Effect |
|---|---|
| `id` | Echoed on the response and on an error object |
| `rules`, `komi` | `Rules::parseRules`, then a komi override |
| `boardXSize`, `boardYSize` | Board dimensions (default 19×19) |
| `initialStones`, `initialPlayer` | Handicap / SGF setup stones, and who moves first |
| `moves` | Move history; `"pass"` accepted |
| `maxVisits` | Per-query search budget. Overrides the config. |
| `analysisPVLen` | PV length (default: config `analysisPVLen`, else 15) |
| `includeOwnership`, `includePolicy` | Opt in to the large arrays. Both default **false**. |
| `overrideSettings` | Any config key, including `humanSLProfile` and the `chosenMove*` knobs |

`overrideSettings` is applied to a *copy* of the engine's config, which is then
reloaded — so a setting a query does not mention keeps its configured value
rather than inheriting whatever the previous query set. An unrecognized key is
an error, not a silent no-op.

Win-rates are reported from the config's `reportAnalysisWinratesAs` perspective
(default `BLACK`), **not** from the side to move. `overrideSettings` may change
it per query.

The query is stateless — the position comes entirely from the request, and the
engine's own board (the one `katago_gtp_command` and `katago_analyze` operate
on) is neither read nor written. Up to `numQueryBots` calls therefore run
concurrently; further callers queue for a bot.

### Analysis — asynchronous

| Function | Description |
|----------|-------------|
| `int katago_analyze_async(KataGoEngine* engine, KataGoAnalysisCallback callback, void* userData, int* outQueryId)` | Snapshots the current position and enqueues it for background analysis. Returns `KATAGO_SUCCESS`, or `KATAGO_ERR_INVALID_ARG` / `KATAGO_ERR_SHUTTING_DOWN` / `KATAGO_ERR_ENGINE`. If `outQueryId` is non-NULL it receives the query ID. |
| `int katago_pending_query_count(KataGoEngine* engine)` | Number of enqueued + in-flight async queries. `0` if `engine` is NULL. |
| `void katago_wait_all_queries(KataGoEngine* engine)` | Block until all pending async queries finish. |
| `int katago_wait_all_queries_timeout(KataGoEngine* engine, int timeoutMs)` | Bounded wait. `KATAGO_SUCCESS` if drained, `KATAGO_ERR_TIMEOUT` on timeout, `KATAGO_ERR_INVALID_ARG` if NULL. Negative `timeoutMs` waits forever. |
| `int katago_set_analysis_workers(KataGoEngine* engine, int numWorkers)` | Set the worker-thread count (`1..16`). **Must be called before the first `katago_analyze_async`.** `KATAGO_ERR_INVALID_ARG` on bad args, `KATAGO_ERR_INVALID_STATE` if workers already started. |

The callback type:

```c
typedef void (KATAGO_CALL *KataGoAnalysisCallback)(int queryId,
                                                   const char* json,
                                                   const char* errorMsg,
                                                   void* userData);
```

- Invoked on a **background worker thread**, not the submitting thread.
- `json` and `errorMsg` are valid **only for the duration of the callback** — copy them
  if you need them later. `errorMsg` is `NULL` on success; `json` is `NULL` on failure.
- Keep the callback short (dispatch heavy work elsewhere) to avoid stalling the queue.

### Board introspection

| Function | Description |
|----------|-------------|
| `int katago_board_x_size(KataGoEngine* engine)` | Current board width. `0` if NULL. |
| `int katago_board_y_size(KataGoEngine* engine)` | Current board height. `0` if NULL. |
| `const char* katago_show_board(KataGoEngine* engine)` | Human-readable board rendering. **Free with `katago_free_string`.** `NULL` if NULL. |

### GTP

| Function | Description |
|----------|-------------|
| `const char* katago_gtp_command(KataGoEngine* engine, const char* command)` | Execute one GTP command and return its response string. **Free with `katago_free_string`.** |

Supported commands: `boardsize`, `clear_board`, `komi`, `play`, `genmove`, `undo`,
`showboard`, `name`, `version`, `protocol_version`, `list_commands`, `known_command`,
`kata-set-rules`, `kata-analyze` / `lz-analyze` / `analyze`, `quit`.

### Utilities

| Function | Description |
|----------|-------------|
| `int katago_api_version(void)` | C ABI version. Consumers should require the version they were compiled against. |
| `const char* katago_version(void)` | KataGo version string. **Static — do NOT free.** |
| `int katago_has_human_model(KataGoEngine* engine)` | `1` if a Human-SL model was loaded, else `0` (also `0` for a NULL engine). |
| `int katago_query_concurrency(KataGoEngine* engine)` | How many `katago_query_json` calls run at once. `0` for a NULL engine. |
| `void katago_free_string(const char* str)` | Free any heap-allocated string returned by a `katago_*()` function. |

---

## Memory & threading rules

- **Free every returned string** with `katago_free_string()` — except `katago_version()`,
  which returns a static buffer.
- **One engine, many threads** — every entry point is internally synchronized and safe
  to call concurrently. Whether calls run *in parallel* depends on which one:
  `katago_query_json` is stateless and runs up to `katago_query_concurrency()` searches
  at once, while the calls that operate on the engine's own board
  (`katago_analyze`, `katago_gtp_command`, the `play`/`undo` family) are serialized
  against each other.
- **No global initialization is required.** The Zobrist and score-value tables that
  `MainCmds` sets up in the executable are initialized on the first engine construction.
- **Async callbacks run on worker threads.** Do not call `katago_destroy()` from inside
  a callback, and never let a C++ exception or foreign-language unwind cross the
  callback boundary. Synchronize access to your own data.
- **Destruction ordering** — `katago_destroy()` sets the shutting-down flag, wakes and
  joins all workers, then releases the bot, NN evaluator, and logger. After destruction,
  the handle is invalid; do not reuse it.
- **Synchronous destruction contract** — before `katago_destroy()`, the caller must
  prevent new calls and wait for all synchronous calls (`katago_query_json`, GTP,
  board operations) to return. The C API cannot make a raw pointer safe against a
  call that races deletion. GoGame enforces this with reference-counted ownership.

---

## Usage examples

### Synchronous analysis (C)

```c
#include "katago_api.h"
#include <stdio.h>

int main(void) {
    const char* err = NULL;
    KataGoEngine* eng = katago_create("model.bin.gz", "analysis.cfg", 0, &err);
    if(!eng) {
        fprintf(stderr, "create failed: %s\n", err ? err : "unknown");
        katago_free_string(err);
        return 1;
    }

    const char* setup = katago_gtp_command(eng, "boardsize 19");
    katago_free_string(setup);

    const char* json = katago_analyze(eng);
    if(json) { printf("%s\n", json); katago_free_string(json); }

    katago_destroy(eng);
    return 0;
}
```

### Raw JSON query

```c
const char* q =
  "{\"id\":\"q1\",\"rules\":\"chinese\",\"komi\":7.5,"
  "\"boardXSize\":19,\"boardYSize\":19,"
  "\"moves\":[[\"B\",\"Q16\"],[\"W\",\"D4\"]]}";
const char* resp = katago_query_json(eng, q);
if(resp) { puts(resp); katago_free_string(resp); }
```

### GTP commands

```c
const char* r1 = katago_gtp_command(eng, "play B D4");   katago_free_string(r1);
const char* r2 = katago_gtp_command(eng, "genmove W");   /* "= Q16" */
printf("KataGo plays: %s\n", r2);                        katago_free_string(r2);
```

### Asynchronous analysis with a callback

```c
#include "katago_api.h"
#include <stdio.h>

static void KATAGO_CALL on_result(int queryId, const char* json,
                                  const char* errorMsg, void* userData) {
    if(errorMsg) fprintf(stderr, "query %d failed: %s\n", queryId, errorMsg);
    else         printf("query %d: %s\n", queryId, json);
}

void run_async(KataGoEngine* eng) {
    katago_set_analysis_workers(eng, 2);   /* before the first submit */

    int id = 0;
    int rc = katago_analyze_async(eng, on_result, NULL, &id);
    if(rc != KATAGO_SUCCESS) { fprintf(stderr, "submit rc=%d\n", rc); return; }

    /* Wait up to 30s for all pending queries to complete. */
    if(katago_wait_all_queries_timeout(eng, 30000) == KATAGO_ERR_TIMEOUT)
        fprintf(stderr, "analysis timed out\n");
}
```

---

## Versioning & ABI notes

- The public surface is `katago_api.h`. Changes are **additive**: new functions and new
  `KataGoError` values are appended; existing signatures and enum values are stable.
- Symbols use `__cdecl` on Windows. Windows export annotations, hidden
  visibility, and the Linux linker version script ensure that only the 19
  explicit `katago_*` C functions are exported; C++ implementation symbols are
  private.
- `katago_api_version()` reports the C ABI version. `katago_version()` reports the
  KataGo release version; these version spaces are intentionally separate.
- `katago_version()` reports the KataGo release version string.
