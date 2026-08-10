# KataGo Shared Library (DLL) — Hardening & Reorganization Plan

This document tracks the work to make the KataGo C shared library (`katago.dll` /
`libkatago.so` / `libkatago.dylib`) more robust, better documented, and better organized. It complements the public API
header at `cpp/lib/katago_api.h`.

## Objectives

1. **Robustness** — remove undefined/fragile behavior in the C API and async engine.
2. **API review** — audit and extend `cpp/lib/katago_api.h` (additive, ABI-stable).
3. **Documentation** — a full reference in `cpp/lib/README.md`.
4. **Organization** — all library sources live under `cpp/lib/`.

## Decisions (locked)

- Library folder: **`cpp/lib/`**.
- Robustness scope: **core hardening R1–R6** (no thread-local `katago_last_error`).
- Verification: **full EIGEN CPU build** via `cpp/build_dll.bat EIGEN`.
- All `katago_api.h` changes are **additive** — no existing symbol signatures change.

## File map (after reorganization)

| File                           | Role                                                        |
|--------------------------------|-------------------------------------------------------------|
| `cpp/lib/katago_api.h`         | Public C API (opaque handle, error codes, functions)        |
| `cpp/lib/katago_api.cpp`       | `extern "C"` adapter + `Version::` definitions              |
| `cpp/lib/katago_engine.h/.cpp` | Internal `KataGoEngine` facade (board + NN + async workers) |
| `cpp/lib/katago_gtp_handler.*` | GTP command dispatcher + registry                           |
| `cpp/lib/README.md`            | Library documentation (to be written)                       |

Build glue: `cpp/CMakeLists.txt` (`BUILD_AS_DLL` block) and `cpp/build_dll.bat`.

## Robustness items

- **R1 — JSON error escaping.** `katago_analyze` / `katago_query_json` build
  `{"error":"<e.what()>"}` by string concatenation; unescaped quotes/backslashes/newlines produce invalid JSON. Fix:
  build the error object with `nlohmann::json`.
- **R2 — Shutdown race.** `katago_analyze_async` returns `SUCCESS` even during teardown. Add an engine `shuttingDown_`
  flag (set first in the destructor) and surface the already-declared `KATAGO_ERR_SHUTTING_DOWN`.
- **R3 — Silent worker-config no-op.** `setAnalysisWorkers` silently ignores calls after workers start; the C wrapper
  still returns `SUCCESS`. Make the engine method return
  `bool`; map failure to new `KATAGO_ERR_INVALID_STATE`.
- **R4 — Unbounded wait.** Add `katago_wait_all_queries_timeout(engine, timeoutMs)` backed by a
  `waitAllQueries(timeoutMs)` overload (`doneCV_.wait_for`); new `KATAGO_ERR_TIMEOUT`.
- **R5 — Board introspection.** Expose existing engine getters via new C entry points
  `katago_board_x_size`, `katago_board_y_size`, `katago_show_board`.
- **R6 — NULL-handle contract.** Audit every function so a NULL engine returns a defined error code where one is
  expected; `katago_destroy(NULL)` stays a safe no-op.

### New error codes

```
KATAGO_ERR_INVALID_STATE = -5   // e.g. setting workers after they started
KATAGO_ERR_TIMEOUT       = -6   // bounded wait expired
```

### New public functions

- `int  katago_board_x_size(KataGoEngine*)`
- `int  katago_board_y_size(KataGoEngine*)`
- `const char* katago_show_board(KataGoEngine*)`   // free with katago_free_string
- `int  katago_wait_all_queries_timeout(KataGoEngine*, int timeoutMs)`

## Build changes

- `cpp/CMakeLists.txt`: point the 6 DLL sources at `lib/…`; add
  `target_include_directories(katago PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/lib)`
  so moved files still resolve `core/…`, `game/…`, `search/…`.
- `cpp/build_dll.bat`: update the header echo/report paths to `lib\katago_api.h`.

## Verification

- Run `cpp/build_dll.bat EIGEN`; the DLL must compile and link cleanly.
- There is no unit-test harness for the C API, so the build is the authoritative check.

## Task checklist

- [x] Move library files into `cpp/lib/`
- [x] Fix include resolution for moved files
- [x] Robustness R1–R6 (engine + adapter)
- [x] Extend `katago_api.h` (error codes + new functions)
- [x] Update `CMakeLists.txt`
- [x] Update `build_dll.bat`
- [x] Write `cpp/lib/README.md`
- [x] Verify full EIGEN build (BUILD SUCCESSFUL)

> Detailed, evolving working notes live in the session plan
> (`~/.copilot/session-state/.../plan.md`); this file is the committed, in-repo summary.

---

# Round 2 — query parity with the analysis engine

Driven by embedding the library in a real application (GoGame), which loads it in place of `katago.exe analysis`. Four
defects made that impossible; all are fixed. `katago_create` keeps its signature, so the ABI is additive.

## Defects fixed

- **G1 — The global tables were never initialized.** `Board::initHash()` and
  `ScoreValue::initTables()` are called by whichever `MainCmds` entry point the executable runs, and the DLL has no
  `main()`. **Every** `katago_create` call aborted the host process on `Failed test assert: IS_ZOBRIST_INITALIZED` — the
  library had evidently only ever been exercised through the thin `katago.exe`, which links `main.cpp`. Fixed with a
  `GlobalTableInit` sentinel declared as the **first** member of `KataGoEngine`: the `Board` members assert inside their
  own constructors, which run before the engine constructor body, so a `call_once`
  in the body would have been too late.
- **G2 — `queryJson` ignored `maxVisits`.** It searched with the engine's construction-time `params_` whatever the
  request said, so a caller could not vary search depth per query. Now parsed and applied to a per-query
  `SearchParams`.
- **G3 — `queryJson` ignored `overrideSettings`, and no human model could be loaded.** `katago_create` passed `""` as
  the human-model argument to
  `Setup::initializeNNEvaluator`, so `humanSLProfile` had nothing to select. Both fixed: `katago_create_ex` takes a
  human model, and overrides are applied to a *copy* of the config which is then reloaded — mirroring
  `command/analysis.cpp`, so an unmentioned setting keeps its configured value instead of inheriting the previous
  query's. Unknown keys are rejected, and a
  `humanSLProfile` with no human model loaded is an error rather than a silent fallback to the ordinary net.
- **G4 — Win-rates used the side-to-move perspective.**
  `reportAnalysisWinratesAs` was never read, so the reported win-rate inverted on every turn.
  `Setup::parseReportAnalysisWinrates` is now consulted at construction and overridable per query; `katago_analyze` and
  the async worker use it too.

## Other changes

- **Concurrency.** `queryJson` no longer holds `mutex_` across the search. Its position comes entirely from the request,
  so it leases an `AsyncBot` from a pool sized by `numAnalysisThreads` (or `numQueryBots`) and several queries run in
  parallel, as they do in the analysis engine.
- **Config compatibility.** `numSearchThreadsPerAnalysisThread` is aliased to
  `numSearchThreads`, and `loadSingleRules` is only consulted when the config actually declares `koRule` — analysis
  configs do not, and it hard-failed on them.
- **Error shape.** Rejections return `{"id":…,"error":…}` so a caller can fail the one query responsible instead of
  guessing.
- **`includeOwnership` / `includePolicy`** are honoured per query (previously hardcoded on) and default to false.

## New public functions

- `KataGoEngine* katago_create_ex(model, humanModel, config, numThreads, numQueryBots, outError)`
- `int katago_has_human_model(KataGoEngine*)`
- `int katago_query_concurrency(KataGoEngine*)`

## Verification

`cpp/build_dll.bat OPENCL` builds clean. A C harness against the real 19×19 net + `b18c384nbt-humanv0` human net
confirms: `maxVisits` respected;
`humanSLProfile` accepted and reflected in `humanWinrate`/`humanScoreMean`; win-rate stays Black-relative across a
change of side to move; handicap via
`initialStones` + `initialPlayer` scores correctly; unknown override keys and malformed JSON both return error objects;
`katago_destroy` returns cleanly.

## Task checklist

- [x] G1 global table initialization
- [x] G2 per-query `maxVisits`
- [x] G3 `overrideSettings` + human model
- [x] G4 win-rate perspective
- [x] Query bot pool for concurrent `queryJson`
- [x] `katago_create_ex` / `katago_has_human_model` / `katago_query_concurrency`
- [x] Verify OPENCL build and run the C harness

---

# Round 3 — ABI and embedding hardening

This working tree is a patch set against upstream KataGo v1.17.1 commit
`5246793f77b480dee91a3b92902d1a9b92860bd0`. Until the work is pushed to a
fork, that base revision plus this repository diff is the reproducible source
of the DLL consumed by GoGame.

## Changes

- Evaluators are owned by `std::unique_ptr` immediately after creation. A later
  constructor failure can no longer leak an already-loaded normal or Human-SL
  evaluator while GoGame retries initialization.
- `katago_query_json` validates replayed moves through
  `makeBoardMoveTolerant`; malformed histories return structured JSON errors
  instead of reaching an `AssumeLegal` path.
- `katago_api_version()` and `KATAGO_API_VERSION` separate C ABI compatibility
  from the KataGo engine release version.
- The MSVC DLL exports only the supported C surface. The earlier build exported
  more than 18,000 C++ implementation symbols because
  `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` was enabled.
- `katago_create_ex` initializes `outError`, rejects negative sizing values,
  and all string-returning exception boundaries have an unknown-exception
  fallback. Allocation failure produces `NULL` rather than unwinding through C.
- `katago_dll_smoke` is a checked-in public-ABI harness. It tests ABI version,
  malformed JSON, illegal history rejection, concurrent real-model queries,
  allocation/free symmetry, and destruction.
- `build_dll.bat` discovers CMake and vcpkg through caller overrides, PATH,
  `VCPKG_ROOT`, or conventional per-user locations; it no longer contains a
  machine-specific absolute username path.

## Verified result

- Backend: OPENCL, Release, AVX2, MSVC 19.38.
- Exported surface: 19 `katago_*` C functions and no C++ symbols.
- Native smoke: normal and Human-SL models, two concurrent one-visit queries,
  structured error cases, and clean shutdown passed on an RTX 3070.
- GoGame must require C ABI version 1 before creating an engine.

## Release prerequisite

Create a maintained fork/branch and commit this patch set before treating the
binary as releasable. Record the fork commit and DLL SHA-256 in GoGame's build
metadata; do not rely on an uncommitted sibling checkout as the long-term source.
