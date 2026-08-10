/**
 * katago_engine.h - Internal engine class (Facade pattern)
 *
 * Encapsulates all KataGo subsystems (NN evaluator, search bot, game state)
 * behind a single cohesive interface. Each instance is fully independent.
 *
 * Responsibilities:
 *   - Owns the lifecycle of Logger, NNEvaluator, AsyncBot, and game state.
 *   - Provides high-level operations: analyze, queryJson, play move, generate move, etc.
 *   - Thread-safe via internal mutex.
 *
 * This is an internal header — not part of the public C API.
 */

#ifndef KATAGO_ENGINE_H
#define KATAGO_ENGINE_H

#include "core/config_parser.h"
#include "core/logger.h"
#include "game/board.h"
#include "game/rules.h"
#include "game/boardhistory.h"
#include "neuralnet/nneval.h"
#include "search/asyncbot.h"
#include "search/searchparams.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Forward-declared so the C API can use an opaque pointer.
struct KataGoEngine {

  // --- Construction / Destruction ---

  // Throws std::runtime_error on failure.
  //
  // humanModelFile may be empty, in which case no human SL model is loaded and
  // queries carrying a humanSLProfile override are rejected.
  //
  // numQueryBots is the number of queryJson() calls that may run concurrently.
  // 0 means "take numAnalysisThreads from the config" (default 1).
  KataGoEngine(const std::string& modelFile,
               const std::string& humanModelFile,
               const std::string& configFile,
               int numThreads,
               int numQueryBots);
  ~KataGoEngine();

  // Non-copyable, non-movable (owns heavy resources).
  KataGoEngine(const KataGoEngine&) = delete;
  KataGoEngine& operator=(const KataGoEngine&) = delete;

  // --- Board manipulation (all thread-safe) ---

  void setBoardSize(int size);
  void clearBoard();
  void setKomi(float komi);
  void setRules(const Rules& rules);

  // Returns true on success. Sets outError on failure.
  bool playMove(Player pla, const std::string& locStr, std::string& outError);

  // Returns true if undo succeeded. Restores initial stones and replays remaining moves.
  bool undoMove();

  // --- Search ---

  // Generate a move for the given player. Returns the move string (e.g. "D4", "pass").
  std::string generateMove(Player pla);

  // Run analysis on the current position. Returns full JSON string with rootInfo, moveInfos, and ownership.
  std::string analyze();

  // Execute a raw JSON analysis query.
  //
  // Honours the analysis-engine query fields that change the answer:
  //   id, rules, komi, boardXSize, boardYSize, initialPlayer, initialStones,
  //   moves, maxVisits, analysisPVLen, includeOwnership, includePolicy,
  //   overrideSettings (including humanSLProfile and the chosenMove* knobs).
  //
  // Unlike the rest of this class the query is stateless: the position comes
  // entirely from the JSON and the engine's own board is neither read nor
  // written, so up to numQueryBots calls run concurrently.
  std::string queryJson(const std::string& queryJsonStr);

  // Whether a human SL model was loaded. Queries that set humanSLProfile are
  // rejected when this is false.
  bool hasHumanModel() const;

  // How many queryJson() calls may run at once.
  int queryConcurrency() const;

  // --- Async analysis ---

  // Callback signature: (queryId, jsonResult, errorMsg)
  using AnalysisCallback = std::function<void(int, const std::string&, const std::string&)>;

  // Submit an async analysis query. Returns the query ID.
  // Takes a snapshot of the current board state — safe to mutate the board after.
  // Throws std::runtime_error if the engine is shutting down.
  int submitAnalysisQuery(AnalysisCallback callback);

  // Number of pending (enqueued + in-flight) queries.
  int pendingQueryCount() const;

  // Block until all pending queries are done.
  void waitAllQueries();

  // Block until all pending queries are done or the timeout elapses.
  // Returns true if all queries completed, false on timeout.
  bool waitAllQueries(int timeoutMs);

  // Set number of worker threads (default 1). Must call before first submitAnalysisQuery.
  // Returns false if workers have already started or count is out of range [1,16].
  bool setAnalysisWorkers(int count);

  // True once the destructor has begun shutting down the async workers.
  bool isShuttingDown() const;

  // --- Queries ---

  std::string showBoard() const;
  int boardXSize() const;
  int boardYSize() const;

private:
  // MUST stay the first member. KataGo's Zobrist and score-value tables are
  // process-global and are normally initialized by the MainCmds entry point the
  // executable ran; the DLL has no main(). The Board members below assert on
  // uninitialized Zobrist tables in their own constructors, which run before
  // this class's constructor body — so the initialization has to happen in a
  // member that is constructed ahead of them.
  struct GlobalTableInit { GlobalTableInit(); };
  GlobalTableInit globalTableInit_;

  mutable std::mutex mutex_;

  // Subsystem ownership
  std::unique_ptr<Logger>      logger_;
  // Setup returns raw pointers, but the engine takes ownership immediately.
  // unique_ptr is important here: if a later constructor step throws, fully
  // constructed members are destroyed even though ~KataGoEngine is not run.
  std::unique_ptr<NNEvaluator> nnEval_;
  std::unique_ptr<NNEvaluator> humanEval_;   // NULL when no human model was given.
  std::unique_ptr<AsyncBot>    bot_;
  SearchParams                 params_;

  // Kept for the lifetime of the engine so per-query overrideSettings can be
  // applied to a copy of the original config, exactly as the analysis engine does.
  std::unique_ptr<ConfigParser> cfg_;
  Player                        perspective_;    // reportAnalysisWinratesAs
  int                           analysisPVLen_;
  bool                          preventEncore_;

  // Game state
  Rules         rules_;
  Board         initialBoard_;
  Player        initialPlayer_;
  Board         board_;
  BoardHistory  history_;
  Player        nextPlayer_;

  // Internal helpers (caller must hold mutex_)
  void syncBotPosition();

  // --- queryJson bot pool ---
  //
  // queryJson() takes its whole position from the request, so it needs a search
  // bot but not the engine's board. Leasing one bot per in-flight query is what
  // keeps concurrent callers from serializing behind mutex_.

  std::mutex                             poolMutex_;
  std::condition_variable                poolCV_;
  std::vector<std::unique_ptr<AsyncBot>> queryBots_;
  std::vector<bool>                      queryBotBusy_;

  // RAII lease on one pooled bot; blocks in the constructor until one is free.
  class QueryBotLease {
  public:
    explicit QueryBotLease(KataGoEngine& engine);
    ~QueryBotLease();
    QueryBotLease(const QueryBotLease&) = delete;
    QueryBotLease& operator=(const QueryBotLease&) = delete;
    AsyncBot* bot() const { return bot_; }
  private:
    KataGoEngine& engine_;
    size_t        index_;
    AsyncBot*     bot_;
  };

  // --- Async worker infrastructure ---

  struct AnalysisQuery {
    int              queryId;
    Board            boardSnapshot;
    BoardHistory     historySnapshot;
    Player           playerSnapshot;
    SearchParams     paramsSnapshot;
    AnalysisCallback callback;
  };

  std::mutex                    queueMutex_;
  std::condition_variable       queueCV_;
  std::queue<AnalysisQuery>     queryQueue_;
  std::atomic<bool>             shutdownWorkers_{false};
  std::atomic<int>              pendingCount_{0};
  std::condition_variable       doneCV_;       // signaled when pendingCount_ reaches 0
  std::vector<std::thread>      workers_;
  int                           desiredWorkers_ = 1;
  bool                          workersStarted_ = false;

  void ensureWorkersStarted();
  void workerLoop();
};

#endif /* KATAGO_ENGINE_H */
