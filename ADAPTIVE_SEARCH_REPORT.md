# Adaptive Search Improvement Report

Status: **Experimental implementation report; not a production-default decision**

> [!NOTE]
> This document details the improvements made to the KataGo adaptive search implementation to satisfy both the engine requirements and clean code (SOLID) principles.

Adaptive search is an optional, capability-discovered shared-library extension.
The measurements below record an implementation iteration, not completion of
the broader validation gate. Production defaults should not change until the
corpus, latency, stability, and regression requirements in
[`docs/SHARED_LIBRARY_ROADMAP.md`](docs/SHARED_LIBRARY_ROADMAP.md#p2--adaptive-search-validation)
pass. Consumer repositories own their own temporary enablement policy.

## 1. Algorithmic Redesign

The initial adaptive search implementation had a major design flaw: it used `Search::setParams()`, which triggered a complete clearance of the search tree. When an ambiguous position was encountered, all prior visit data was thrown away, causing a massive latency spike as the engine rebuilt the tree from scratch up to double the visit count.

### Iterative Deepening
We migrated to a **progressive iterative deepening** approach using `setParamsNoClearing()`.
- Instead of immediately jumping to `initialVisits * 2`, the loop incrementally expands the search budget by 25% increments.
- Because the tree is preserved, the engine rapidly searches these additional increments without wasting GPU effort on already-explored paths.
- The loop terminates early the moment the ambiguity resolves, dramatically reducing the median latency ratio against the baseline from `1.80x` down to `1.24x`.

## 2. Refined Ambiguity Heuristic

Previously, ambiguity was defined strictly by checking if the raw winrates of the top two moves were within `0.03`. This fails in endgame scenarios where winrates often compress to `99.0%` vs `99.2%`, even if the score difference is 15 points.

We shifted to evaluating the **Utility Lower Confidence Bound (`utilityLcb`)**:
- KataGo internally ranks moves using utility, which mathematically blends both the winrate and the expected score lead.
- By looking at the `utilityLcb` delta (e.g., `< 0.04`), we implicitly factor in search variance and ensure that adaptive search only triggers when KataGo itself genuinely considers the value of two moves to be virtually indistinguishable.

## 3. SOLID & Clean Code Architecture

In the C++ KataGo fork (`katago_engine.cpp`), the logic checking for ambiguity was heavily coupled with the engine control flow, violating the **Single Responsibility Principle**.

To fix this, the mathematical condition was extracted into an encapsulated static lambda helper `isPositionAmbiguous()`.
- The main `query` loop is now exclusively responsible for managing the iterative deepening control flow.
- The `isPositionAmbiguous()` helper is exclusively responsible for parsing the JSON `moveInfos`, resolving utility metrics, and returning the boolean state.
- This adheres strictly to clean code guidelines, making it extensible without touching the core query pipeline.

## 4. Example TengenGo Backend Integration

At the time of this implementation report, the adaptive-search toggle was also
wired into the downstream Rust backend (`crates/bot/src/lib.rs` and `gtp.rs`).
These settings document that consumer revision; they are not shared-library
defaults and do not replace the validation gate above.

- **Dynamic Bot Tiers**: Lower-tier opponents (`Beginner`, `Developing`, `Club`, `Advanced`) explicitly disable adaptive search, as allocating extra search budget to an intentionally flawed opponent is wasteful.
- **Consumer Tier Policy**: `Master` and `Superhuman` tiers passed `adaptive_search: true` dynamically in that revision.
- **Analysis Integration**: The interactive game review layer set `adaptive_search: true` in that revision. This is experimental consumer policy, not evidence that the larger quality/latency evaluation has passed.
