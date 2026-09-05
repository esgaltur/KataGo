# KataGo Shared Library Roadmap

Status: **Canonical roadmap for the embeddable C API**

The KataGo shared library (`katago.dll`, `libkatago.so`, or
`libkatago.dylib`) is a general-purpose embedding interface. It is not tied to
one application. Any host that can call a C ABI may use it, including C, C++,
Rust, C#, Python, Go, and Java/Kotlin applications.

TengenGo/GoGame is the first production consumer and a valuable integration
test, but consumer-specific scheduling, persistence, UI behavior, and product
policy do not belong in the public KataGo API.

The public contract is [`cpp/lib/katago_api.h`](../cpp/lib/katago_api.h). The
usage and build guide is [`cpp/lib/README.md`](../cpp/lib/README.md). Historical
implementation and platform investigation notes remain in
[`DLL_Library_Plan.md`](DLL_Library_Plan.md).

## Mission

- Embed KataGo without starting and supervising a command-line subprocess.
- Expose a flat, language-neutral C ABI with explicit ownership and lifecycle
  rules.
- Support stateful GTP-style use and stateless JSON analysis.
- Make concurrency, cancellation, shutdown, and failures bounded and
  observable.
- Keep application policy outside the library.
- Minimize changes to upstream KataGo so upgrades remain reviewable.

## Non-goals

- The library does not provide process isolation. A fatal native failure also
  terminates an in-process host.
- The library does not own application-level request priority, authentication,
  persistence, retries, or user-facing difficulty policy.
- Experimental search behavior is not enabled implicitly by the ABI.
- A shared-library build does not remove the runtime requirements of its
  selected Eigen, OpenCL, CUDA, TensorRT, ROCm, or ONNX backend.

## Compatibility contract

The current public contract is ABI **v1.2**:

- `KATAGO_API_VERSION == 1` is the incompatible-change boundary.
- `KATAGO_API_VERSION_MINOR == 2` identifies additive ABI-v1 extensions.
- The original 19 functions are the required ABI-v1 base surface.
- The current library exports 27 `katago_*` functions: 19 required base
  functions and 8 additive functions.
- Optional behavior is discovered with `katago_api_capabilities()`; consumers
  must not infer a capability from the engine release number.
- Existing signatures, enum values, calling conventions, and ownership rules
  remain stable throughout ABI v1.
- A larger, versioned options structure is accepted only when its documented
  prefix is understood; unknown suffix bytes are ignored.

The seven currently advertised capabilities are:

| Capability | Contract |
| --- | --- |
| `KATAGO_CAP_CREATE_OPTIONS` | Versioned construction options, including async worker and queue bounds |
| `KATAGO_CAP_QUERY_CANCELLATION` | Cooperative cancellation of an active stateless JSON query by request ID |
| `KATAGO_CAP_TELEMETRY_CONTEXT` | Telemetry registration with host context and quiescent clearing |
| `KATAGO_CAP_BOUNDED_ASYNC_QUEUE` | Explicitly bounded stateful async-analysis admission |
| `KATAGO_CAP_STRICT_HISTORY` | Strict player order and legality validation for replayed JSON histories |
| `KATAGO_CAP_ADAPTIVE_SEARCH` | Opt-in experimental adaptive-search query fields and response metadata |
| `KATAGO_CAP_BUILD_INFO` | Static JSON identity for the engine release, fork revision, upstream baseline, backend, ABI, and capabilities |

Adding a capability does not necessarily add a function. Strict-history and
adaptive-search support, for example, extend the JSON query contract.

## Current baseline

| Area | Status |
| --- | --- |
| Source baseline | Upstream KataGo v1.18.2 is merged into `feature/gogame-shared-library-api` |
| Required ABI | v1, 19 base functions |
| Current additive revision | v1.2, 27 total exported functions and 7 capability bits |
| Language boundary | Pure C header with opaque engine handles |
| Stateless analysis | Concurrent bounded bot pool, structured JSON errors, optional Human-SL model |
| Safety extensions | Construction options, bounded async queue, cancellation, strict history, quiescent telemetry clearing |
| Platform evidence | Windows OpenCL and Linux Eigen/OpenCL have prior real-model verification records |
| v1.18.2 release certification | **In progress**; do not treat the merge alone as a certified artifact |
| Adaptive search | **Experimental**; capability-discovered and opt-in at the library boundary |
| V3D OpenCL optimization | **Planned experiment**; Eigen remains the measured Raspberry Pi 5 recommendation for the tested model |

## Roadmap

### P0 — Certify the v1.18.2 baseline

- Complete the post-merge source adaptations without changing ABI v1.
- Build Windows OpenCL and Eigen libraries and Linux Eigen/OpenCL libraries.
- Run the pure-C link smoke and the real-model public-ABI smoke.
- Verify the 19-function base contract, all 27 current exports, and the absence
  of leaked C++ implementation symbols.
- Exercise Human-SL loading, malformed input, illegal and strict histories,
  concurrent queries, cancellation, telemetry quiescence, bounded queueing,
  and destruction.
- Run at least one downstream consumer through the exact packaged artifact.
- Preserve a known-good rollback artifact until certification completes.

Acceptance: the source revision, built library, model, configuration, export
surface, and smoke results are recorded together and can be reproduced.

### P0 — Release identity and integrity

Status: **In progress** — the self-describing ABI manifest is implemented;
artifact checksums and signed release metadata remain.

- Completed in the runtime manifest:
  - KataGo release and upstream commit;
  - fork commit and dirty-tree state;
  - ABI major/minor and capability bitset;
  - compiled neural-network backend.
- Extend the runtime or companion signed release manifest with:
  - target OS, architecture, compiler, and build type;
  - library, model, Human-SL model, and configuration SHA-256 values;
  - required and optional export lists.
- Make packaging fail when the manifest, artifact, or checksums disagree.
- Load the packaged library and execute a real query as a release gate.
- Keep application-specific metadata outside the library manifest.

Acceptance: a consumer can identify exactly which native implementation and
assets it loaded without relying on filenames or adjacent repository state.

### P1 — Complete the public SDK documentation

- Keep `cpp/lib/README.md` synchronized with every public declaration in
  `katago_api.h`.
- Add small checked examples for C, Python/ctypes, Rust, and C# or Java/JNA.
- Document model/config compatibility, backend runtime dependencies, callback
  lifetime, shutdown ordering, and process-failure boundaries.
- Document required-base versus optional-capability symbol loading.
- Provide install/export rules for the header, library, and CMake package
  metadata where practical.

Acceptance: a consumer with no knowledge of TengenGo can build, load, query,
cancel, and shut down the library from the public documentation alone.

### P1 — Expand native validation

- Add deterministic tests that do not require a neural network for argument,
  lifecycle, strict-history, queue, and capability behavior.
- Retain real-model tests for evaluator construction and actual search.
- Add malformed JSON and boundary-value fuzzing at the public C entry points.
- Stress concurrent query/cancel/destroy coordination under sanitizers on
  supported toolchains.
- Test old ABI-v1 consumers against the current library and current consumers
  against a base-v1 library with optional symbols absent.

Acceptance: invalid or adversarial input produces a documented error and does
not unwind through C, leak ownership, deadlock, or grow work without a bound.

### P1 — Reduce and isolate the fork

- Keep shared-library adaptation under `cpp/lib` where upstream interfaces
  allow it.
- Move consumer-driven adaptive orchestration out of central search code and
  into the library adapter while preserving search-tree reuse.
- Keep OpenCL compatibility changes isolated from the C ABI and from adaptive
  search.
- Remove refactors that are not required for the embedding surface.
- Prepare generic, independently reviewable changes for possible upstream
  submission.

Acceptance: ordinary upstream executable behavior is unchanged unless a
shared-library caller explicitly requests an extension.

### P2 — Query-aware observability

- Add engine and query identity to telemetry without breaking the existing
  callback functions.
- Distinguish admission wait, search, neural-network evaluation, cancellation,
  and completion.
- Report requested and effective visits for bounded or adaptive searches.
- Define callback concurrency and data-lifetime rules before adding a new
  capability bit.

Acceptance: concurrent queries and multiple engines can be attributed without
embedding application-specific tracing types in the ABI.

### P2 — Predictable work budgets

- Evaluate an explicit per-query time ceiling in addition to `maxVisits`.
- Preserve a deterministic visit ceiling for tests and offline analysis.
- Keep application-level priority and fairness in the host unless a generic
  native scheduling primitive is justified by measurements.
- Measure cooperative-cancellation latency and bot-pool release time.

Acceptance: interactive consumers can bound native work without making review
or benchmark results irreproducible.

### P2 — Adaptive-search validation

- Keep adaptive search opt-in and capability-discovered.
- Add deterministic coverage for disabled, unambiguous, tree-reuse, maximum
  budget, overflow, and cancellation paths.
- Evaluate at least 500 representative positions with paired seeds, warmups,
  randomized order, and repeated trials.
- Measure reference move agreement, win-rate and score error, median and p95
  latency, refinement rate, and memory.
- Do not change defaults based on isolated positions or tuner scores.

Acceptance: a documented quality improvement meets a predeclared latency and
stability budget.

### P3 — Board-size and backend performance

- Establish comparable end-to-end Eigen and OpenCL baselines first.
- Attribute time to kernels before changing implementations.
- Test exact 9x9 and 13x13 evaluator geometry against the current 19x19 buffer.
- Continue only if whole-query improvement justifies evaluator ownership and
  memory cost.
- Treat V3D/Rusticl tuning, fusion, INT8, and driver work as separate measured
  experiments with desktop regression coverage.

The detailed Raspberry Pi/V3D protocol and stop conditions remain in
[`DLL_Library_Plan.md`](DLL_Library_Plan.md#round-5--v3d-opencl-performance-investigation).

## Release gate

Every published shared-library artifact should pass:

- source revision and clean/dirty-state capture;
- pure-C header and link smoke;
- exact required/optional export validation;
- real normal-model query and optional Human-SL query;
- structured malformed-input and illegal-history checks;
- concurrency, cancellation, queue bound, telemetry clear, and shutdown checks;
- string allocation/free symmetry;
- target-backend warm start on deployment hardware;
- manifest and checksum verification;
- a downstream load/query smoke using only the packaged files.

Generated models, tuning files, traces, credentials, and build trees remain
machine-local unless a release process explicitly packages them with their
licenses and provenance.

## Document ownership

- This file owns future shared-library priorities and status.
- `cpp/lib/README.md` owns the public usage and API guide.
- `cpp/lib/katago_api.h` is the normative ABI declaration.
- `DLL_Library_Plan.md` owns historical implementation notes and the detailed
  V3D experiment design.
- `ADAPTIVE_SEARCH_REPORT.md` records the adaptive-search experiment; it does
  not set production defaults.
- Consumer repositories own their integration policy, deployment limits, and
  product behavior.
