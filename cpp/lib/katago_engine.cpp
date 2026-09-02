#include "katago_engine.h"
#include "core/global.h"
#include "core/rand.h"
#include "core/mainargs.h"
#include "program/setup.h"
#include "external/nlohmann_json/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "core/using.h"

#include "telemetry.h"

namespace {

// Directory part of a path, or "" if it has none.
std::string directoryOf(const std::string& path) {
  size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

std::string formatAnalysisJson(Search* search, Player perspective) {
  if(!search) return "{\"error\": \"null search\"}";
  nlohmann::json analysisJson;
  Search::AnalysisJsonOptions options;
  options.analysisPVLen = 15;
  options.preventEncore = false;
  options.includePolicy = true;
  options.includeOwnership = true;
  
  bool suc = search->getAnalysisJson(
    perspective,
    options,
    analysisJson
  );
  if(suc) {
    return analysisJson.dump();
  }

  ReportedSearchValues values;
  if(!search->getRootValues(values))
    return "{\"error\": \"search failed\"}";

  std::ostringstream out;
  out << "{"
      << "\"winrate\": "   << values.winValue << ", "
      << "\"scoreLead\": " << values.lead     << ", "
      << "\"utility\": "   << values.utility
      << "}";
  return out.str();
}

void validateAdaptiveSearchParams(const SearchParams& params) {
  if(!std::isfinite(params.adaptiveVisitRatio) ||
     params.adaptiveVisitRatio < 0.0 || params.adaptiveVisitRatio > 1.0)
    throw std::runtime_error("adaptiveVisitRatio must be between 0 and 1");
  if(!std::isfinite(params.adaptiveUtilityTolerance) || params.adaptiveUtilityTolerance < 0.0)
    throw std::runtime_error("adaptiveUtilityTolerance must be non-negative");
  if(!std::isfinite(params.adaptiveMaxMultiplier) || params.adaptiveMaxMultiplier < 1.0)
    throw std::runtime_error("adaptiveMaxMultiplier must be at least 1");
  if(!std::isfinite(params.adaptiveStepMultiplier) || params.adaptiveStepMultiplier <= 0.0)
    throw std::runtime_error("adaptiveStepMultiplier must be greater than 0");
}

// Errors are reported in the analysis-engine's shape: an object carrying the
// query id when there is one, so the caller can fail exactly the request that
// went wrong instead of guessing. nlohmann::json does the escaping.
std::string makeQueryErrorJson(const std::string& id, const std::string& message) {
  nlohmann::json j;
  if(!id.empty()) j["id"] = id;
  j["error"] = message;
  return j.dump();
}

} // anonymous namespace

// Runs before every other member of every engine instance; see the declaration
// in katago_engine.h for why it has to.
KataGoEngine::GlobalTableInit::GlobalTableInit() {
  static std::once_flag sOnce;
  std::call_once(sOnce, []{
    Board::initHash();
    ScoreValue::initTables();
  });
}

KataGoEngine::KataGoEngine(const std::string& modelFile,
                           const std::string& humanModelFile,
                           const std::string& configFile,
                           int numThreads,
                           int numQueryBots,
                           int numAsyncWorkers,
                           int asyncQueueCapacity)
  : nnEval_(nullptr)
  , humanEval_(nullptr)
  , perspective_(P_BLACK)
  , analysisPVLen_(15)
  , preventEncore_(true)
  , assumeMultipleStartingBlackMovesAreHandicap_(true)
  , initialPlayer_(P_BLACK)
  , nextPlayer_(P_BLACK)
{
  if(numAsyncWorkers < 1 || numAsyncWorkers > 16)
    throw std::runtime_error("numAsyncWorkers must be between 1 and 16");
  if(asyncQueueCapacity < 1 || asyncQueueCapacity > 65536)
    throw std::runtime_error("asyncQueueCapacity must be between 1 and 65536");
  desiredWorkers_ = numAsyncWorkers;
  asyncQueueCapacity_ = (size_t)asyncQueueCapacity;

  cfg_ = std::make_unique<ConfigParser>(configFile);
  ConfigParser& cfg = *cfg_;

  // Analysis configs express the per-query search width under a different name;
  // the parameter loader only knows "numSearchThreads".
  cfg.applyAlias("numSearchThreadsPerAnalysisThread", "numSearchThreads");

  if(numThreads > 0)
    cfg.overrideKey("numSearchThreads", Global::intToString(numThreads));

  // KataGo derives its data directory (GPU tuning caches, downloaded nets) from
  // the *host executable's* location — GetModuleFileNameW(NULL) on Windows,
  // /proc/self/exe on Linux. For a shared library that is the embedding
  // application, not KataGo, so a distribution shipping a pre-tuned KataGoData
  // beside its model would never find it and every fresh install would pay a
  // full OpenCL autotune. Default it to the model's own directory, which is
  // where that cache sits in every KataGo layout. An explicit homeDataDir in
  // the config still wins.
  if(!cfg.contains("homeDataDir")) {
    const std::string modelDir = directoryOf(modelFile);
    if(!modelDir.empty())
      cfg.overrideKey("homeDataDir", modelDir + "/KataGoData");
  }

  logger_ = std::make_unique<Logger>(nullptr, false, false, true, false);

  Setup::initializeSession(cfg);

  const bool hasHumanModel = !humanModelFile.empty();
  params_      = Setup::loadSingleParams(cfg, Setup::SETUP_FOR_ANALYSIS, hasHumanModel);
  perspective_ = Setup::parseReportAnalysisWinrates(cfg, P_BLACK);

  // Analysis configs carry no ruleset — every query names its own — and
  // loadSingleRules hard-fails on a config without koRule. Only consult the
  // config when it actually declares rules; otherwise take KataGo's default,
  // which is what the analysis engine does for a query that omits "rules".
  rules_ = cfg.contains("koRule") ? Setup::loadSingleRules(cfg, true) : Rules();

  if(cfg.contains("analysisPVLen"))
    analysisPVLen_ = cfg.getInt("analysisPVLen", 1, 100);
  if(cfg.contains("preventCleanupPhase"))
    preventEncore_ = cfg.getBool("preventCleanupPhase");
  if(cfg.contains("assumeMultipleStartingBlackMovesAreHandicap"))
    assumeMultipleStartingBlackMovesAreHandicap_ = cfg.getBool("assumeMultipleStartingBlackMovesAreHandicap");

  // How many queryJson() calls may run at once. Matches the analysis engine's
  // numAnalysisThreads so a config tuned for katago.exe behaves the same here.
  if(numQueryBots > 0)
    numQueryBots = std::min(numQueryBots, 64);
  else if(cfg.contains("numAnalysisThreads"))
    numQueryBots = cfg.getInt("numAnalysisThreads", 1, 64);
  else
    numQueryBots = 1;

  int threads = (numThreads > 0 ? numThreads : params_.numThreads);
  const int concurrentSearches = numQueryBots + numAsyncWorkers + 1;
  int expectedConcurrentEvals = std::max(threads * concurrentSearches, 4);
  int defaultMaxBatchSize = std::max(8, ((expectedConcurrentEvals + 3) / 4) * 4);

  Rand seedRand;
  nnEval_.reset(Setup::initializeNNEvaluator(
    modelFile, modelFile, string(), cfg, *logger_, seedRand,
    expectedConcurrentEvals,
    NNPos::MAX_BOARD_LEN,
    NNPos::MAX_BOARD_LEN,
    defaultMaxBatchSize,
    false,
    false,
    Setup::SETUP_FOR_ANALYSIS
  ));

  if(hasHumanModel) {
    humanEval_.reset(Setup::initializeNNEvaluator(
      humanModelFile, humanModelFile, string(), cfg, *logger_, seedRand,
      expectedConcurrentEvals,
      NNPos::MAX_BOARD_LEN,
      NNPos::MAX_BOARD_LEN,
      defaultMaxBatchSize,
      false,
      false,
      Setup::SETUP_FOR_ANALYSIS
    ));
    if(!humanEval_->requiresSGFMetadata()) {
      logger_->write(
        "WARNING: the human model was not trained from SGF metadata to vary by rank; "
        "humanSLProfile will have no effect. Was the wrong file passed as the human model?"
      );
    }
  }

  board_        = Board(19, 19);
  initialBoard_ = board_;
  nextPlayer_   = P_BLACK;
  initialPlayer_= P_BLACK;
  history_      = BoardHistory(
    board_, nextPlayer_, rules_, 0,
    Search::resolveAlwaysComputePassAliveUnderSuicideRules(params_, nnEval_.get())
  );
  history_.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);

  Rand botRand;
  bot_ = std::make_unique<AsyncBot>(
    params_, nnEval_.get(), humanEval_.get(), logger_.get(),
    Global::uint64ToString(botRand.nextUInt64())
  );
  bot_->setAlwaysIncludeOwnerMap(true);

  queryBots_.reserve((size_t)numQueryBots);
  for(int i = 0; i < numQueryBots; i++) {
    queryBots_.emplace_back(std::make_unique<AsyncBot>(
      params_, nnEval_.get(), humanEval_.get(), logger_.get(),
      Global::uint64ToString(botRand.nextUInt64())
    ));
  }
  queryBotBusy_.assign((size_t)numQueryBots, false);
  activeQueryIds_.assign((size_t)numQueryBots, std::string());
}

// --- Query bot pool ---

KataGoEngine::QueryBotLease::QueryBotLease(KataGoEngine& engine, const std::string& queryId)
  : engine_(engine), index_(0), bot_(nullptr)
{
  std::unique_lock<std::mutex> lock(engine_.poolMutex_);
  engine_.poolCV_.wait(lock, [this]{
    for(size_t i = 0; i < engine_.queryBotBusy_.size(); i++) {
      if(!engine_.queryBotBusy_[i]) {
        index_ = i;
        return true;
      }
    }
    return false;
  });
  engine_.queryBotBusy_[index_] = true;
  engine_.activeQueryIds_[index_] = queryId;
  bot_ = engine_.queryBots_[index_].get();
}

KataGoEngine::QueryBotLease::~QueryBotLease() {
  {
    std::lock_guard<std::mutex> lock(engine_.poolMutex_);
    engine_.queryBotBusy_[index_] = false;
    engine_.activeQueryIds_[index_].clear();
  }
  engine_.poolCV_.notify_one();
}

bool KataGoEngine::cancelJsonQuery(const std::string& queryId) {
  if(queryId.empty())
    return false;
  std::lock_guard<std::mutex> lock(poolMutex_);
  for(size_t i = 0; i < activeQueryIds_.size(); i++) {
    if(queryBotBusy_[i] && activeQueryIds_[i] == queryId) {
      queryBots_[i]->stopWithoutWait();
      return true;
    }
  }
  return false;
}

bool KataGoEngine::hasHumanModel() const {
  return humanEval_ != nullptr;
}

int KataGoEngine::queryConcurrency() const {
  return (int)queryBots_.size();
}

KataGoEngine::~KataGoEngine() {
  // Shut down async workers first.
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    shutdownWorkers_ = true;
  }
  queueCV_.notify_all();
  for(auto& t : workers_) {
    if(t.joinable()) t.join();
  }
  workers_.clear();

  // Every bot holds a pointer into the evaluators, so all of them must be gone
  // before the nets are released.
  queryBots_.clear();
  bot_.reset();
  humanEval_.reset();
  nnEval_.reset();
  logger_.reset();
  cfg_.reset();
}

// --- Board manipulation ---

void KataGoEngine::setBoardSize(int size) {
  std::lock_guard<std::mutex> lock(mutex_);
  board_        = Board(size, size);
  initialBoard_ = board_;
  nextPlayer_   = P_BLACK;
  initialPlayer_= P_BLACK;
  history_      = BoardHistory(
    board_, nextPlayer_, rules_, 0,
    Search::resolveAlwaysComputePassAliveUnderSuicideRules(params_, nnEval_.get())
  );
  history_.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
  syncBotPosition();
}

void KataGoEngine::clearBoard() {
  std::lock_guard<std::mutex> lock(mutex_);
  board_        = Board(board_.x_size, board_.y_size);
  initialBoard_ = board_;
  nextPlayer_   = P_BLACK;
  initialPlayer_= P_BLACK;
  history_      = BoardHistory(
    board_, nextPlayer_, rules_, 0,
    Search::resolveAlwaysComputePassAliveUnderSuicideRules(params_, nnEval_.get())
  );
  history_.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
  syncBotPosition();
}

void KataGoEngine::setKomi(float komi) {
  std::lock_guard<std::mutex> lock(mutex_);
  rules_.komi = komi;
  history_.setKomi(komi);
}

bool KataGoEngine::setRules(const Rules& requestedRules, std::string& outError) {
  std::lock_guard<std::mutex> lock(mutex_);
  Rules rules = requestedRules;
  rules.komi = rules_.komi;

  bool rulesWereSupported;
  nnEval_->getSupportedRules(rules, rulesWereSupported);
  if(!rulesWereSupported) {
    outError = "rules are not supported by the loaded neural network";
    return false;
  }

  Board replayBoard = initialBoard_;
  BoardHistory replayHistory(
    replayBoard,
    initialPlayer_,
    rules,
    0,
    Search::resolveAlwaysComputePassAliveUnderSuicideRules(params_, nnEval_.get())
  );
  replayHistory.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
  Player replayPlayer = initialPlayer_;
  const std::vector<Move> moves = history_.moveHistory;
  for(const Move& move : moves) {
    if(move.pla != replayPlayer) {
      replayBoard.clearSimpleKoLoc();
      replayHistory.clear(replayBoard, move.pla, rules, replayHistory.encorePhase);
      replayHistory.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
    }
    if(!replayHistory.makeBoardMoveTolerant(replayBoard, move.loc, move.pla, preventEncore_)) {
      outError = "an earlier move cannot be replayed under the requested rules";
      return false;
    }
    replayPlayer = getOpp(move.pla);
  }

  rules_ = rules;
  board_ = std::move(replayBoard);
  history_ = std::move(replayHistory);
  nextPlayer_ = replayPlayer;
  syncBotPosition();
  return true;
}

bool KataGoEngine::playMove(Player pla, const std::string& locStr, std::string& outError) {
  std::lock_guard<std::mutex> lock(mutex_);

  Loc loc = Board::PASS_LOC;
  if(locStr != "pass" && locStr != "PASS") {
    loc = Location::ofString(locStr, board_);
  }

  if(pla != nextPlayer_) {
    board_.clearSimpleKoLoc();
    history_.clear(board_, pla, rules_, history_.encorePhase);
    history_.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
  }
  if(!history_.makeBoardMoveTolerant(board_, loc, pla, preventEncore_)) {
    outError = "illegal move";
    return false;
  }

  nextPlayer_ = getOpp(pla);
  syncBotPosition();
  return true;
}

bool KataGoEngine::undoMove() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(history_.moveHistory.empty())
    return false;

  // Replay all moves except the last one starting from the initial board state.
  std::vector<Move> moves = history_.moveHistory;
  moves.pop_back();

  board_      = initialBoard_;
  nextPlayer_ = initialPlayer_;
  history_    = BoardHistory(
    board_, nextPlayer_, rules_, 0,
    Search::resolveAlwaysComputePassAliveUnderSuicideRules(params_, nnEval_.get())
  );
  history_.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);

  for(const Move& m : moves) {
    if(m.pla != nextPlayer_) {
      board_.clearSimpleKoLoc();
      history_.clear(board_, m.pla, rules_, history_.encorePhase);
      history_.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
    }
    if(!history_.makeBoardMoveTolerant(board_, m.loc, m.pla, preventEncore_))
      throw std::runtime_error("undo replay failed");
    nextPlayer_ = getOpp(m.pla);
  }
  syncBotPosition();
  return true;
}

// --- Search ---

std::string KataGoEngine::generateMove(Player pla) {
  TRACE_SPAN("generateMove");
  std::lock_guard<std::mutex> lock(mutex_);
  syncBotPosition();
  Loc moveLoc = bot_->genMoveSynchronous(pla, TimeControls());

  if(moveLoc == Board::NULL_LOC)
    return "";

  history_.makeBoardMoveAssumeLegal(board_, moveLoc, pla, nullptr);
  nextPlayer_ = getOpp(pla);

  if(moveLoc == Board::PASS_LOC)
    return "pass";

  return Location::toString(moveLoc, board_);
}

std::string KataGoEngine::analyze() {
  TRACE_SPAN("analyze");
  std::lock_guard<std::mutex> lock(mutex_);
  syncBotPosition();
  bot_->genMoveSynchronous(nextPlayer_, TimeControls());

  Search* search = bot_->getSearchStopAndWait();
  // Reported from the configured perspective (reportAnalysisWinratesAs), not
  // from the side to move — otherwise the winrate flips every turn.
  return formatAnalysisJson(search, perspective_);
}

std::string KataGoEngine::queryJson(const std::string& queryJsonStr) {
  TRACE_SPAN("queryJson");
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(queryJsonStr);
  } catch(const std::exception& e) {
    return makeQueryErrorJson("", std::string("Invalid JSON: ") + e.what());
  }

  if(!req.is_object())
    return makeQueryErrorJson("", "request must be a JSON object");

  std::string id;
  try {
    if(req.contains("id")) {
      if(!req["id"].is_string())
        return makeQueryErrorJson("", "id must be a string");
      id = req["id"].get<std::string>();
    }
  } catch(const std::exception& e) {
    return makeQueryErrorJson("", e.what());
  }

  try {
    // ---- Search parameters -------------------------------------------------
    //
    // Per-query settings are what make one engine serve every difficulty tier.
    // They are resolved the same way the analysis engine resolves them: apply
    // the overrides to a *copy* of the original config and reload, so a value
    // the caller did not mention keeps its configured default rather than
    // whatever the previous query left behind.

    SearchParams params      = params_;
    Player       perspective = perspective_;

    if(req.contains("overrideSettings")) {
      if(!req["overrideSettings"].is_object())
        throw std::runtime_error("overrideSettings must be an object");

      std::map<std::string,std::string> overrides;
      for(auto it = req["overrideSettings"].begin(); it != req["overrideSettings"].end(); ++it)
        overrides[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();

      if(!overrides.empty()) {
        ConfigParser localCfg(*cfg_);
        // Only keys the *override* introduced should be reported as unknown.
        localCfg.markAllKeysUsedWithPrefix("");
        localCfg.overrideKeys(overrides);
        params      = Setup::loadSingleParams(localCfg, Setup::SETUP_FOR_ANALYSIS, hasHumanModel());
        perspective = Setup::parseReportAnalysisWinrates(localCfg, perspective_);
        SearchParams::failIfParamsDifferOnUnchangeableParameter(params_, params);

        std::vector<std::string> unusedKeys = localCfg.unusedKeys();
        if(!unusedKeys.empty())
          throw std::runtime_error("Unknown overrideSettings params: " + Global::concat(unusedKeys, ","));

        // A humanSLProfile with no human model loaded would silently produce
        // ordinary KataGo moves; the caller asked for something we cannot give.
        std::ostringstream warning;
        if(Setup::maybeWarnHumanSLParams(params, nnEval_.get(), humanEval_.get(), warning, nullptr))
          throw std::runtime_error(warning.str());
      }
    }

    if(req.contains("maxVisits")) {
      if(!req["maxVisits"].is_number_integer())
        throw std::runtime_error("maxVisits must be an integer");
      int64_t maxVisits = req["maxVisits"].get<int64_t>();
      if(maxVisits < 1)
        throw std::runtime_error("maxVisits must be at least 1");
      params.maxVisits = maxVisits;
    }

    if(req.contains("adaptiveSearch"))
      params.adaptiveSearch = req["adaptiveSearch"].get<bool>();
    if(req.contains("adaptiveVisitRatio"))
      params.adaptiveVisitRatio = req["adaptiveVisitRatio"].get<double>();
    if(req.contains("adaptiveUtilityTolerance"))
      params.adaptiveUtilityTolerance = req["adaptiveUtilityTolerance"].get<double>();
    if(req.contains("adaptiveMaxMultiplier"))
      params.adaptiveMaxMultiplier = req["adaptiveMaxMultiplier"].get<double>();
    if(req.contains("adaptiveStepMultiplier"))
      params.adaptiveStepMultiplier = req["adaptiveStepMultiplier"].get<double>();
    validateAdaptiveSearchParams(params);

    const int  analysisPVLen    = req.contains("analysisPVLen")
                                    ? req["analysisPVLen"].get<int>() : analysisPVLen_;
    if(analysisPVLen < 1 || analysisPVLen > 1000)
      throw std::runtime_error("analysisPVLen must be between 1 and 1000");
    const bool includeOwnership = req.value("includeOwnership", false);
    const bool includePolicy    = req.value("includePolicy", false);

    // ---- Position ----------------------------------------------------------
    //
    // Built entirely from the request. The engine's own board is untouched,
    // which is what lets several queries run at once.

    const int bx = req.value("boardXSize", 19);
    const int by = req.value("boardYSize", 19);
    if(bx < 2 || bx > Board::MAX_LEN || by < 2 || by > Board::MAX_LEN)
      throw std::runtime_error("board size out of range");

    Rules r = rules_;
    if(req.contains("rules")) {
      if(req["rules"].is_string())
        r = Rules::parseRules(req["rules"].get<std::string>());
      else if(req["rules"].is_object())
        r = Rules::parseRules(req["rules"].dump());
      else
        throw std::runtime_error("rules must be a rules name or detailed rules object");
    }
    if(req.contains("komi")) {
      if(!req["komi"].is_number())
        throw std::runtime_error("komi must be numeric");
      const double komi = req["komi"].get<double>();
      if(!std::isfinite(komi) || komi < Rules::MIN_USER_KOMI || komi > Rules::MAX_USER_KOMI ||
         !Rules::komiIsIntOrHalfInt((float)komi))
        throw std::runtime_error("komi must be an integer or half-integer from -400 to 400");
      r.komi = (float)komi;
    }
    if(req.contains("whiteHandicapBonus")) {
      if(!req["whiteHandicapBonus"].is_string())
        throw std::runtime_error("whiteHandicapBonus must be a string");
      r.whiteHandicapBonusRule = Rules::parseWhiteHandicapBonusRule(
        req["whiteHandicapBonus"].get<std::string>()
      );
    }

    bool rulesWereSupported;
    Rules supportedRules = nnEval_->getSupportedRules(r, rulesWereSupported);
    const std::string rulesWarning = rulesWereSupported
      ? std::string()
      : "requested rules are unsupported by the neural network; compatible rules were used";
    r = supportedRules;

    Board b(bx, by);
    Player pla = P_BLACK;

    if(req.contains("initialPlayer")) {
      if(!req["initialPlayer"].is_string())
        throw std::runtime_error("initialPlayer must be \"B\" or \"W\"");
      Player parsed;
      if(!PlayerIO::tryParsePlayer(req["initialPlayer"].get<std::string>(), parsed))
        throw std::runtime_error("initialPlayer must be \"B\" or \"W\"");
      pla = parsed;
    }

    if(req.contains("initialStones") && !req["initialStones"].is_array())
      throw std::runtime_error("initialStones must be an array");
    if(req.contains("initialStones")) {
      for(const auto& item : req["initialStones"]) {
        if(!item.is_array() || item.size() != 2)
          throw std::runtime_error("each initialStones entry must be [player, location]");
        Player p;
        if(!PlayerIO::tryParsePlayer(item[0].get<std::string>(), p))
          throw std::runtime_error("bad player in initialStones");
        Loc loc = Location::ofString(item[1].get<std::string>(), b);
        if(loc == Board::PASS_LOC || !b.setStone(loc, p))
          throw std::runtime_error("invalid location in initialStones");
      }
    }

    if(!req.contains("initialPlayer")) {
      if(req.contains("moves") && req["moves"].is_array() && !req["moves"].empty()) {
        const auto& firstMove = req["moves"][0];
        if(!firstMove.is_array() || firstMove.size() != 2 || !firstMove[0].is_string() ||
           !PlayerIO::tryParsePlayer(firstMove[0].get<std::string>(), pla))
          throw std::runtime_error("bad first move while inferring initialPlayer");
      }
      else
        pla = BoardHistory::numHandicapStonesOnBoard(b) > 0 ? P_WHITE : P_BLACK;
    }

    BoardHistory hist(
      b, pla, r, 0,
      Search::resolveAlwaysComputePassAliveUnderSuicideRules(params, nnEval_.get())
    );
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);

    size_t numMoves = 0;
    if(req.contains("moves") && !req["moves"].is_array())
      throw std::runtime_error("moves must be an array");
    const bool strictHistory = req.value("strictHistory", false);
    if(req.contains("moves")) {
      for(const auto& item : req["moves"]) {
        if(!item.is_array() || item.size() != 2)
          throw std::runtime_error("each moves entry must be [player, location]");
        Player p;
        if(!PlayerIO::tryParsePlayer(item[0].get<std::string>(), p))
          throw std::runtime_error("bad player in moves");
        std::string locStr = item[1].get<std::string>();
        Loc loc = (locStr == "pass" || locStr == "PASS")
                    ? Board::PASS_LOC : Location::ofString(locStr, b);
        // Match the analysis executable: tolerate an explicit change of player
        // by clearing simple-ko history, but reject genuinely illegal moves
        // instead of passing untrusted input to an AssumeLegal operation.
        if(strictHistory && p != pla)
          throw std::runtime_error("move " + Global::uint64ToString(numMoves) + " has the wrong player");
        if(!strictHistory && p != pla) {
          b.clearSimpleKoLoc();
          hist.clear(b, p, r, hist.encorePhase);
          hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap_);
        }
        const bool moveSucceeded = strictHistory
          ? hist.isLegal(b, loc, p)
          : hist.isLegalTolerant(b, loc, p);
        if(!moveSucceeded)
          throw std::runtime_error(
            "illegal move " + Global::uint64ToString(numMoves) + ": " + locStr
          );
        hist.makeBoardMoveAssumeLegal(b, loc, p, nullptr, preventEncore_);
        pla = getOpp(p);
        numMoves++;
      }
    }

    // ---- Search ------------------------------------------------------------
    QueryBotLease lease(*this, id);
    AsyncBot* bot = lease.bot();

    bot->setParams(params);
    bot->setAlwaysIncludeOwnerMap(includeOwnership);
    bot->setPosition(pla, b, hist);
    bot->genMoveSynchronous(pla, TimeControls());

    Search* search = bot->getSearchStopAndWait();
    if(!search)
      throw std::runtime_error("search failed: null search");

    nlohmann::json resp;
    Search::AnalysisJsonOptions options;
    options.analysisPVLen = analysisPVLen;
    options.preventEncore = preventEncore_;
    options.includePolicy = includePolicy;
    options.includeOwnership = includeOwnership;

    bool suc = search->getAnalysisJson(
      perspective,
      options,
      resp
    );

    if(params.adaptiveSearch && search->lastSearchUsedAdaptiveExtension && suc) {
      resp["adaptiveSearch"] = true;
      resp["adaptiveSearchInitialVisits"] = params.maxVisits;
      resp["adaptiveSearchFinalVisits"] = search->getRootVisits();
    }

    if(!suc) {
      ReportedSearchValues values;
      if(!search->getRootValues(values))
        throw std::runtime_error("search failed: no root values");
      resp["rootInfo"]["winrate"]   = values.winValue;
      resp["rootInfo"]["scoreLead"] = values.lead;
      resp["rootInfo"]["utility"]   = values.utility;
    }

    if(!id.empty()) resp["id"] = id;
    if(!rulesWarning.empty()) resp["warning"] = rulesWarning;
    resp["turnNumber"] = numMoves;
    return resp.dump();
  }
  catch(const std::exception& e) {
    return makeQueryErrorJson(id, e.what());
  }
}

// --- Queries ---

std::string KataGoEngine::showBoard() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Board::toStringSimple(board_, ' ');
}

int KataGoEngine::boardXSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return board_.x_size;
}

int KataGoEngine::boardYSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return board_.y_size;
}

// --- Async analysis ---

bool KataGoEngine::setAnalysisWorkers(int count) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if(workersStarted_ || count < 1 || count > 16)
    return false;
  desiredWorkers_ = count;
  return true;
}

bool KataGoEngine::isShuttingDown() const {
  return shutdownWorkers_.load();
}

int KataGoEngine::submitAnalysisQuery(AnalysisCallback callback) {
  if(shutdownWorkers_.load())
    throw KataGoShuttingDownError();

  AnalysisQuery query;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    static std::atomic<int> sNextId{1};
    query.queryId         = sNextId++;
    query.boardSnapshot   = board_;
    query.historySnapshot = history_;
    query.playerSnapshot  = nextPlayer_;
    query.paramsSnapshot  = params_;
    query.callback        = std::move(callback);
  }

  const int queryId = query.queryId;
  ensureWorkersStarted();

  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if(shutdownWorkers_.load())
      throw KataGoShuttingDownError();
    if(queryQueue_.size() >= asyncQueueCapacity_)
      throw KataGoQueueFullError();
    queryQueue_.push(std::move(query));
    pendingCount_++;
  }
  queueCV_.notify_one();
  return queryId;
}

int KataGoEngine::pendingQueryCount() const {
  return pendingCount_.load();
}

void KataGoEngine::waitAllQueries() {
  std::unique_lock<std::mutex> lock(queueMutex_);
  doneCV_.wait(lock, [this]{ return pendingCount_.load() == 0; });
}

bool KataGoEngine::waitAllQueries(int timeoutMs) {
  if(timeoutMs < 0) {
    waitAllQueries();
    return true;
  }
  std::unique_lock<std::mutex> lock(queueMutex_);
  return doneCV_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this]{ return pendingCount_.load() == 0; });
}

void KataGoEngine::ensureWorkersStarted() {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if(workersStarted_) return;
  workersStarted_ = true;
  for(int i = 0; i < desiredWorkers_; i++) {
    workers_.emplace_back(&KataGoEngine::workerLoop, this);
  }
}

void KataGoEngine::workerLoop() {
  // Create a persistent AsyncBot for this worker thread to avoid reallocating on every query
  Rand botRand;
  AsyncBot workerBot(
    params_, nnEval_.get(), humanEval_.get(), logger_.get(),
    Global::uint64ToString(botRand.nextUInt64())
  );
  workerBot.setAlwaysIncludeOwnerMap(true);

  while(true) {
    AnalysisQuery query;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCV_.wait(lock, [this]{ return shutdownWorkers_ || !queryQueue_.empty(); });
      if(shutdownWorkers_ && queryQueue_.empty())
        return;
      query = std::move(queryQueue_.front());
      queryQueue_.pop();
    }

    std::string jsonResult;
    std::string errorMessage;
    try {
      workerBot.setParams(query.paramsSnapshot);
      workerBot.setPosition(query.playerSnapshot, query.boardSnapshot, query.historySnapshot);
      workerBot.genMoveSynchronous(query.playerSnapshot, TimeControls());

      Search* search = workerBot.getSearchStopAndWait();
      if(!search)
        errorMessage = "search failed: null search";
      else
        jsonResult = formatAnalysisJson(search, perspective_);
    } catch(const std::exception& e) {
      errorMessage = e.what();
    } catch(...) {
      errorMessage = "unknown native exception";
    }

    // A callback is invoked at most once. Foreign-language callbacks must not
    // unwind, but contain a C++ exception as a last line of defense so the
    // worker and pending-query accounting remain live.
    try {
      query.callback(query.queryId, jsonResult, errorMessage);
    } catch(...) {}

    pendingCount_--;
    doneCV_.notify_all();
  }
}

// --- Internal ---

void KataGoEngine::syncBotPosition() {
  bot_->setPosition(nextPlayer_, board_, history_);
}
