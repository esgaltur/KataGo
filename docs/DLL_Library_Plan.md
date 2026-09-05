# KataGo Shared Library (DLL) — Hardening & Reorganization Plan

Status: **Historical implementation record plus the detailed V3D experiment plan**

The canonical forward roadmap for the general-purpose shared library is
[`SHARED_LIBRARY_ROADMAP.md`](SHARED_LIBRARY_ROADMAP.md). This document retains
the completed implementation rounds and the detailed Raspberry Pi/V3D
measurement protocol. Export counts in completed verification records describe
the ABI revision tested at that time; the current ABI v1.2 surface has 19
required base functions and 27 total exported functions.

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

Future shared-library work is tracked in the committed, repository-visible
[`SHARED_LIBRARY_ROADMAP.md`](SHARED_LIBRARY_ROADMAP.md).

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

This patch set includes upstream KataGo v1.18.2 commit
`fd0723fdbc0e9d82cf269c9630af8c27c57c07c4` and is maintained on the
`feature/gogame-shared-library-api` branch of
`https://github.com/esgaltur/KataGo`.

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
- Every consumer must require C ABI version 1 before creating an engine.
  GoGame/TengenGo is one example consumer.

## Release prerequisite

Record the exact fork commit and native-library SHA-256 in release metadata.
A release binary must be reproducible from the maintained fork rather than
from an uncommitted checkout. Consumer packages, including GoGame/TengenGo,
should validate those values rather than infer them from a filename.

---

# Round 4 — OpenCL tuning without device profiling

## Problem

Mesa Rusticl on the Raspberry Pi 5 V3D device exposes working OpenCL compute
queues but rejects `CL_QUEUE_PROFILING_ENABLE` with
`CL_INVALID_QUEUE_PROPERTIES`. KataGo previously treated this as a fatal error
before the tuner could validate or select any kernels. Merely switching to
`clCreateCommandQueueWithProperties` does not help: the same driver rejects the
same property through both APIs.

## Design

- `cpp/neuralnet/openclhelpers.cpp` first requests the original profiling
  queue. Only `CL_INVALID_QUEUE_PROPERTIES`/`CL_INVALID_VALUE` trigger a retry
  with an ordinary in-order queue and a visible warning.
- `cpp/neuralnet/opencltuner.cpp` records a monotonic host timestamp immediately
  before each measured enqueue. It first drains the queue so the host interval
  has the same work boundary as OpenCL event timestamps.
- After the event completes, device timestamps remain authoritative when they
  are available. When both timestamp queries return
  `CL_PROFILING_INFO_NOT_AVAILABLE`, elapsed host time is substituted.
- Unexpected profiling errors, enqueue errors, execution errors, and failed
  output/reference comparisons retain their original failure behavior.
- The tune-file version and format, candidate space, correctness tolerances,
  FP16 capability probing, and normal inference path do not change. This is not
  a C ABI change; `KATAGO_API_VERSION` remained 1 and the artifact tested in
  this round retained its then-current 19-function export list.

## Rejected alternatives

- **Use the modern queue constructor:** V3D returned `-35` for both legacy and
  modern profiling constructors.
- **Always use built-in defaults:** a diagnostic proved inference could run,
  but it bypassed KataGo's correctness comparisons and was therefore removed.
- **Pretend tuning succeeded from a foreign tuning file:** tuning data is
  device/model/board/tuner-version specific and must not be copied blindly.
- **Disable OpenCL errors globally:** this would hide real driver or kernel
  failures and corrupt the tuner's selection contract.

## Timing-quality and tune-provenance limitations

The fallback restores compatibility and preserves the tuner's correctness
comparisons, but host-clock timing is not equivalent to OpenCL event timing.
It includes queue draining, command submission, driver scheduling, the final
wait, and ordinary operating-system jitter. It is therefore an approximate
kernel-selection signal, particularly for short kernels, and must not be
described as authoritative device execution time.

The existing tuner already repeats candidates and uses zero-weight warm-up
dispatches in several tuning paths. It accumulates weighted elapsed time; it
does not currently select by median, trimmed mean, or explicit outlier
rejection. A future host-timing hardening change should evaluate robust sample
aggregation without changing device-profiled behavior.

The current version-13 tune format and generated filename identify the tuner
version, device name, geometry, and relevant model dimensions, but not the
timing source or driver version. Normal `loadOrAutoTune` startup loads a valid
existing tune rather than overwriting it. Provenance is nevertheless
ambiguous after an explicit retune because device-profiled and host-timed
results can use the same filename. If timing-source metadata is added, change
the tune-format version and define compatibility explicitly; do not append an
unparsed ad hoc field to the fixed version-13 structure.

Follow-up requirements for Round 4 are therefore:

- extend the fallback warning to state that host-timed tuning may be less
  accurate than device-profiled tuning;
- record `device-profiling` or `host-clock` in a versioned tune format before
  relying on tune provenance for comparisons;
- preserve an existing valid tune during ordinary startup, as the current
  loader already does, and require an explicit retune to replace it;
- retain warm-up and repetition, and evaluate median or trimmed-mean selection
  for host-timed candidates if measurement noise affects repeatability.

## Verification record — 2026-08-15

Raspberry Pi 5, Ubuntu 26.04, AArch64, kernel `7.0.0-1009-raspi`, Mesa/Rusticl
26.0.3, V3D 7.1.7.0, GoGame `b10c384h6nbttflrs` model:

- unmodified ARM64 OpenCL library compiled and exported the exact 19-function
  ABI, then failed its real smoke at profiling queue creation;
- a three-case OpenCL probe returned `0` for a normal queue and `-35` for both
  profiling constructors;
- a temporary conservative-default diagnostic passed the real smoke twice and
  was then removed;
- the host-clock fallback built, tested 55 xGemmDirect, 69 xGemm, 45 transform,
  109 untransform, 104 pooling, 120 attention, 68 transformer RMSNorm, 20
  pointwise, 12 channel-bias, and 25 spatial RMSNorm configurations, while
  rejecting unsupported FP16 modes;
- the tuner saved version-13 parameters and the full C ABI/model smoke passed
  in 33:05.54 with 719,216 KiB peak RSS and no swap;
- the saved-tune warm smoke passed in 2:05.16 with 280,264 KiB peak RSS;
- the final OpenCL library remained AArch64 with exactly 19 exports;
- the Eigen ARM64 comparison passed in 1.29 seconds with 192,764 KiB peak RSS;
- Windows `build_dll.bat OPENCL` built all 125 steps, and the RTX 3070 real
  smoke loaded its existing device-timestamp tuning file and passed API 1.

The fallback is therefore compatible, but V3D OpenCL is not a performance win
for this model. Persist the tuning directory reported by the engine across
runtime recreation. The deployed shared-library path uses model-adjacent
`KataGoData/opencltuning`; the standalone smoke used
`~/.katago/opencltuning`. Benchmark the warm path before choosing this backend.
Generated tuning files must remain machine-local.

---

# Round 5 — V3D OpenCL performance investigation

Status: **planned**. This is an evidence-first optimization project, not a
commitment to make the Raspberry Pi GPU the recommended backend.

Engineering review incorporated: **2026-08-16**. Approve only the comparable
baseline, kernel attribution, and isolated exact-9x9 experiment initially.
Unless they produce a large, reproducible end-to-end improvement, Eigen remains
the recommended backend for this Pi/V3D/transformer-model combination.

## Objective and baseline

Reduce interactive 9x9 inference latency on Raspberry Pi 5 while preserving
the ordinary OpenCL path, numerical correctness, API version 1, all 19 required
base functions, and the current 26-function export surface.

Verified baseline on 2026-08-15:

| Workload | Result |
| --- | --- |
| Model | `b10c384h6nbttflrs`, FP32, transformer, 384 channels |
| NN geometry | Fixed 19x19 buffer even for a 9x9 game |
| Saved-tune ABI smoke | 2:05.16 wall time, 280,264 KiB peak RSS |
| GoGame warm-up | 124.106 seconds for two visits |
| GoGame real move | 124.118 seconds native query; 124.250 seconds end to end |
| Normal level-4 budget | 150 visits; exceeded the 300-second application deadline |
| Temporary deployment cap | Two visits |
| Eigen comparison | 1.29-second ABI/model smoke; an identical GoGame query baseline is still required |

The fixed geometry is a concrete KataGo-side inefficiency. `NNEvaluator` is
constructed with one `nnXLen`/`nnYLen`, and the OpenCL transformer uses its
padded area as the attention sequence length. For a 9x9 position, changing the
geometry from 19x19 to 9x9 reduces spatial elements by `361/81 = 4.46x` and
attention pairs by `(361/81)^2 = 19.86x`. These ratios are upper bounds for the
affected operations, not promises for whole-query speedup.

Relevant source anchors at this baseline are:

- `cpp/program/setup.cpp:115` selects the evaluator's fixed `nnXLen` and
  `nnYLen` from the configured maximum board-buffer size;
- `cpp/neuralnet/openclbackend.cpp:2536` assigns `paddedNNXYLen` to the
  transformer `seqLen`;
- `cpp/neuralnet/openclbackend.cpp:568` stores that padded spatial size on the
  OpenCL compute handle.

The empty-board warm-up and the later one-move query each required about 124.1
seconds in the same process. The repeated cost rules out one-time first-query
program compilation as the primary explanation. Per-query compilation, lazy
construction, and cache behavior remain possible until P1 measures program
construction separately.

The observed V3D capability boundary is one reported compute unit, preferred
workgroup multiple 16, 32 KiB local memory, no `cl_khr_fp16`, no subgroup
extension, and no image support. A driver change may improve code generation
or expose a feature that the hardware genuinely implements, but software
cannot erase the physical throughput ceiling.

## Proposal evaluation summary

This table is the review entry point. “Upside” is a hypothesis to test, not a
promised result.

| Priority | Proposal | Problem class | Expected upside | Implementation cost | Main risk | Initial decision |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | P0 comparable Eigen/OpenCL baseline | Decision prerequisite | No direct speedup; establishes the real deployment gap | Low | Mismatched lifecycle or search budget produces a false comparison | **Required first** |
| 1 | P1 kernel attribution | Measurement prerequisite | No direct speedup; prevents optimizing the wrong layer | Medium | Diagnostic synchronization distorts timings | **Required before code optimization** |
| 2 | P2 exact 9x9 geometry | Definite wasted KataGo work | High for 9x9, especially transformer attention | Low for config experiment; high for automatic multi-size ownership | Extra evaluator memory and lifecycle complexity | **Best first optimization experiment** |
| 3 | P3 V3D-specific tuning | Likely generic tuner/device mismatch | Medium, uncertain until P1 | Medium–high | Overfitting V3D or regressing desktop OpenCL | **Proceed only for measured dominant kernels** |
| 4 | P4 kernel fusion | Dispatch and global-memory overhead | Medium if P1 finds many small hotspots | High | Numerical drift and backend maintenance | **Conditional** |
| 5 | P5 INT8 kernels | FP32 throughput/bandwidth limitation | Potentially high only if packed integer dot products are accelerated | Very high | Quantization changes policy/value accuracy | **Feasibility probe only** |
| 6 | P6 Mesa/Rusticl fixes | Compiler/driver limitation | Unknown | Very high and upstream-dependent | Hardware may not support the desired feature | **Last, with a minimal reproducer** |

Review recommendation: approve P0, P1, and the isolated P2 experiment together.
Defer production multi-size ownership, specialized kernels, quantization, and
Mesa changes until their preceding evidence gate passes.

## Engineering review and approved refinements

The plan is technically sound, conservative, and appropriately skeptical. It
becomes unjustified only if the evidence gates are bypassed and P3-P6 are
implemented before P1/P2 establish a useful path. Round 4 is approved as a
compatibility fallback; it enables evaluation but does not accelerate ordinary
inference, which already uses a non-profiling queue.

The following review findings are accepted:

- obtain an apples-to-apples Eigen baseline using the same GoGame process,
  model, 9x9 positions, visits, query API, and lifecycle as OpenCL;
- label host-clock tuning approximate and distinguish its provenance from
  device-profiled tuning in a versioned format before provenance-based policy
  is added;
- use P1 tracing only for attribution and make every performance acceptance
  decision from tracing-disabled end-to-end runs;
- treat fixed 9x9 deployment configuration separately from automatic
  multi-size evaluator ownership: a low-complexity configuration improvement
  need not reach 2x, while multi-size ownership must;
- record CPU governor/frequency and V3D clock/utilization when the platform
  exposes them, in addition to temperature, throttling, and memory;
- define the minimum acceptable visit budget or playing-strength target before
  describing any backend as deployable. Two visits remains diagnostic only;
- treat the numeric gates in this plan as maintenance-cost decision heuristics,
  not physical or statistical laws.

The review also produced three qualifications that must remain documented:

- the tuner already repeats candidates and includes explicit zero-weight
  warm-up calls in multiple paths; robust outlier-resistant aggregation is the
  missing improvement, not basic repetition;
- ordinary `loadOrAutoTune` startup already returns a valid saved tune instead
  of silently overwriting it; timing-source provenance is still missing for an
  explicit retune;
- repeated 124-second same-process queries rule out one-time first-query
  compilation only as the primary explanation. Per-query compilation, lazy
  construction, or cache behavior remains possible until P1 measures program
  construction separately.

## Constraints and non-goals

- Do not weaken the existing CPU-reference output comparisons or tune-file
  validation.
- Do not force FP16 when `cl_khr_fp16` is absent.
- Do not treat a foreign or renamed tune file as verified without a complete
  correctness smoke for that board/model/device tuple.
- Do not change the public C ABI unless an internal implementation is proven
  impossible; prefer an internal evaluator selection strategy.
- Diagnostic synchronization may be expensive, but it must be gated and have
  zero per-kernel synchronization overhead when disabled.
- The profiling-queue compatibility fallback remains independent. Supporting
  profiling events in Rusticl would improve measurement quality but would not
  by itself accelerate ordinary inference.

## Reproducible benchmark protocol

Every performance comparison must record:

- KataGo commit and `libkatago.so` SHA-256;
- model name/SHA-256, config SHA-256, tune-file SHA-256, and NN geometry;
- Mesa/Rusticl, kernel, firmware, and compiler versions;
- effective visits, batch size, search threads, and query concurrency;
- three fresh-process warm-ups and at least five same-process queries;
- median, minimum, maximum, peak RSS, CPU time, temperature, throttling state,
  CPU governor/frequency, and V3D clock/utilization when exposed by the host;
- whether the result passed the existing reference/ABI smoke;
- both an empty 9x9 position and the deployed one-move position.

Run one experiment at a time on an otherwise idle host. A result is not
comparable if the Pi throttled, another GPU workload ran, the model/config
changed unintentionally, or the output correctness check did not pass.

## Work packages

### P0 — Establish comparable CPU and GPU baselines

Before changing OpenCL code, run Eigen and OpenCL through the same GoGame
workload: identical library API, model and config, empty and one-move 9x9
positions, visit count, process lifecycle, timeout policy, and query
concurrency. Record startup, warm-up, native-query, and end-to-end latency
separately. Do not use the existing 1.29-second Eigen ABI smoke as though it
were an end-to-end comparison.

Acceptance gate:

- both backends complete the same correctness-checked workload;
- raw samples and median/minimum/maximum are retained;
- the effective search budget and returned result semantics are identical;
- the result establishes the performance gap that later packages must close.

### P1 — Attribute time to kernels

Add an opt-in diagnostic mode, tentatively `openclTraceKernelTiming`, around
OpenCL inference dispatches. Because V3D cannot provide event timestamps, the
diagnostic path may drain the in-order queue, start a monotonic host timer,
enqueue one operation, wait for completion, and aggregate the result.

Record per kernel/operation:

- invocation count, total time, minimum, maximum, and mean;
- global/local dimensions and effective batch/board/channel sizes;
- bytes transferred where the buffer sizes are known;
- program compilation time separately from execution time.

Do not log every dispatch by default. Emit one sorted summary at query end so
the trace remains usable.

Acceptance gate:

- tracing disabled produces no added `clFinish` and no measurable regression;
- tracing enabled accounts for at least 95% of query wall time;
- the top operations and dispatch count are recorded for both 19x19 and 9x9;
- the ordinary smoke and output/reference comparisons still pass.

P1 timings are diagnostic attribution only. Queue draining serializes work and
can change dispatch behavior. All claimed speedups and continuation decisions
must be reproduced with tracing disabled using the end-to-end P0 protocol.

### P2 — Prove exact 9x9 geometry, then design size specialization

First use an exact 9x9 config and a newly generated 9x9 tune file. Enable
per-board-size retuning or select an explicit 9x9 tune path so that the tuner
does not reuse a compatible full-board tune. This is a controlled experiment
before changing evaluator ownership. Compare it with the 19x19-buffer baseline
using P1 traces, then reproduce the end-to-end result with tracing disabled.

If exact geometry is valuable, prototype an internal evaluator/search-bot pool
keyed by supported geometry (9x9, 13x13, 19x19), created lazily. The design must
address model-weight/OpenCL-buffer duplication, independent NN caches, query
pool concurrency, and destruction while work is active. An alternative is for
GoGame to own one engine handle per enabled board size; measure memory before
choosing between the two designs.

Acceptance gate:

- exact 9x9 produces equivalent legal policy/value output within existing
  backend tolerances;
- 13x13 and 19x19 remain correct and cannot be routed to undersized buffers;
- a fixed 9x9-only deployment configuration may be retained for any meaningful,
  reproducible gain if it adds no evaluator-ownership complexity;
- the 9x9 median improves by at least 2x before adding automatic multi-size
  evaluator ownership;
- total resident memory remains acceptable on the 8 GiB target.

### P3 — Expand tuning for V3D

Use P1 to optimize only the dominant kernels. Query and record
`CL_KERNEL_WORK_GROUP_SIZE`, preferred workgroup multiple, local memory, and
private-memory/register-pressure proxies for every candidate when available.
Extend the tuner with V3D-relevant candidates rather than hardcoding a tune:

- workgroup dimensions centered on the reported preferred multiple of 16;
- smaller tiles and reduced local-memory usage;
- batch-1 and exact 9x9 shapes;
- alternatives that reduce register pressure and compiler spilling;
- explicit categorization of build, enqueue, execution, correctness, and
  performance rejection.

Acceptance gate:

- every selected candidate passes the existing CPU-reference comparison;
- the saved tune remains versioned and device/model/geometry specific;
- the dominant kernel or full query improves by at least 20% median;
- RTX 3070 and other normal OpenCL paths retain their existing tuner behavior.

### P4 — Fuse dispatch and memory-bound operations

Proceed only if P1 shows material time in small dispatches or intermediate
global-memory traffic. Candidate fusions include bias plus activation,
residual add plus activation, RMSNorm plus projection preparation, and adjacent
transformer pointwise operations. Q/K/V preparation is also a candidate if its
separate dispatches or intermediate buffers appear among the measured
hotspots. Do not fuse blindly: start with the highest aggregate trace cost and
keep an unfused path for comparison.

Acceptance gate:

- fused and unfused outputs satisfy the existing numerical tolerances;
- dispatch count and measured traffic fall as intended;
- the full-query median improves by at least 15%;
- the fused path is capability/shape gated and does not regress desktop GPUs.

### P5 — Test an INT8 feasibility path

V3D advertises `cl_khr_integer_dot_product`, but advertisement alone does not
prove acceleration. Query the packed 4x8-bit capability and acceleration
properties first. Stop this package if the relevant signed/accumulating form is
not accelerated.

If it is accelerated, build a standalone GEMM/convolution microbenchmark before
changing model execution. A later prototype may quantize weights per output
channel at load time, use explicit activation scales, accumulate into INT32,
and dequantize before nonlinear/residual operations.

Acceptance gate:

- the microbenchmark beats the tuned FP32 kernel by at least 2x;
- policy ordering, value/score outputs, ownership, and full-game behavior meet
  an agreed accuracy suite across a representative position corpus;
- quantized execution is opt-in until cross-platform evidence is sufficient;
- unsupported devices stay on the unchanged FP32 path.

### P6 — Isolate and report Rusticl/V3D compiler or driver defects

Move to Mesa only after a minimal kernel or API reproducer attributes a problem
outside KataGo. Candidate driver work includes profiling-queue/event timestamp
support and pathological code generation, spilling, barrier lowering, or
vector scalarization for a dominant kernel. FP16/subgroup exposure is valid
only if hardware support exists and the implementation passes the applicable
conformance tests.

For every suspected driver issue, retain:

- a standalone OpenCL reproducer independent of KataGo;
- expected versus observed output and timing;
- device/compiler dumps needed by Mesa developers;
- a correctness comparison with llvmpipe or another conformant device;
- the upstream Mesa issue/MR reference and the exact patched Mesa commit used
  for any rerun.

Acceptance gate: a patched driver must improve the standalone reproducer and
the complete KataGo query without introducing correctness failures.

## Decision gates and stop conditions

| Gate | Decision |
| --- | --- |
| P1 cannot attribute 95% of wall time | Fix measurement before optimizing |
| Exact 9x9 gives a reproducible low-complexity gain below 2x | A fixed 9x9 deployment may use it; do not add multi-size evaluator complexity |
| A kernel change improves less than 15–20% | Do not retain backend-specific complexity |
| INT8 capability is not accelerated | Stop INT8 work |
| Two-visit 9x9 remains above 30 seconds after P2–P4 | Classify this model/device pair as non-interactive and keep Eigen as deployment default |
| Median reaches 10 seconds or less with correctness intact | Continue toward higher-visit/gameplay validation |

Even if the 10-second target is met, compare against Eigen before recommending
OpenCL. The final decision uses end-to-end latency, strength at an equal time
budget, memory, thermals, stability, and maintenance cost—not backend identity.
The percentage and latency thresholds above are engineering heuristics for
controlling maintenance cost. They do not replace examination of raw samples,
variance, correctness, or user-visible value.

## Per-experiment decision record

Each completed experiment adds one row to this document's verification record:

| Field | Required value |
| --- | --- |
| Hypothesis | The one bottleneck or inefficiency being tested |
| Patch scope | Exact files/functions/config keys changed |
| Controlled baseline | Commit, hashes, geometry, workload, and environment |
| Correctness | ABI/export check, reference comparison, and query/game result |
| Performance | Raw samples plus median/min/max and percentage change |
| Resource effect | Peak RSS, CPU, temperature, throttling, available clock/utilization telemetry, and tune time |
| Cross-device result | Pi V3D plus required Windows/desktop OpenCL regression |
| Decision | `keep`, `revise`, `drop`, or `escalate-to-driver`, with reason |

A change is not “faster” based on one run, tuner score alone, or a lower visit
count. The workload and effective search budget must be identical.

## Required regression matrix

- Pi V3D/Rusticl: exact 9x9 trace, tune, ABI smoke, and real query;
- Pi Eigen: same-workload end-to-end comparison baseline;
- Windows RTX 3070 OpenCL: device-timestamp tuner and real-model smoke;
- 9x9, 13x13, and 19x19 request routing and buffer-size rejection tests;
- API version 1, all 19 required base functions, exactly 27 current public
  `katago_*` exports, and no leaked C++ symbols on Windows and Linux;
- concurrent queries, timeout/abandoned-wait behavior, and bounded destruction;
- no committed tuning files, model binaries, traces, or generated build trees.

## Planned order

- [ ] P0 same-workload Eigen and OpenCL end-to-end baseline
- [ ] P1 kernel-level attribution; use tracing only for diagnosis
- [ ] P2 exact 9x9 experiment
- [ ] P2 evaluator ownership decision, only if the 2x gate passes
- [ ] P3 V3D-specific tuner improvements
- [ ] P4 trace-driven fusion
- [ ] P5 INT8 capability microbenchmark
- [ ] P6 minimal Mesa reproducers and upstream work
- [ ] Repeat deployment game and update GoGame's canonical deployment record
