#include "katago_gtp_handler.h"
#include "katago_engine.h"
#include "game/board.h"
#include "neuralnet/nninputs.h"
#include "main.h"

#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "core/using.h"

namespace {

// GTP response helpers
std::string gtpSuccess(const std::string& msg = "") {
  return "= " + msg;
}
std::string gtpError(const std::string& msg) {
  return "? " + msg;
}

// Parse player color from string. Returns P_BLACK/P_WHITE or throws.
Player parseColor(const std::string& s) {
  if(s == "B" || s == "b" || s == "black" || s == "BLACK")
    return P_BLACK;
  if(s == "W" || s == "w" || s == "white" || s == "WHITE")
    return P_WHITE;
  throw std::runtime_error("invalid color");
}

// Command handler signature: takes engine + remaining args stream, returns GTP response.
using Handler = std::function<std::string(KataGoEngine&, std::istringstream&)>;

std::string handleBoardSize(KataGoEngine& engine, std::istringstream& args) {
  int size = 0;
  if(!(args >> size) || size < 2 || size > NNPos::MAX_BOARD_LEN)
    return gtpError("invalid board size");
  engine.setBoardSize(size);
  return gtpSuccess();
}

std::string handleClearBoard(KataGoEngine& engine, std::istringstream&) {
  engine.clearBoard();
  return gtpSuccess();
}

std::string handleKomi(KataGoEngine& engine, std::istringstream& args) {
  float komi = 0.0f;
  if(!(args >> komi) || !std::isfinite(komi))
    return gtpError("invalid komi");
  engine.setKomi(komi);
  return gtpSuccess();
}

std::string handlePlay(KataGoEngine& engine, std::istringstream& args) {
  std::string colorStr, locStr;
  args >> colorStr >> locStr;
  Player pla;
  try {
    pla = parseColor(colorStr);
  } catch(...) {
    return gtpError("invalid color");
  }
  std::string err;
  if(!engine.playMove(pla, locStr, err))
    return gtpError(err);
  return gtpSuccess();
}

std::string handleGenmove(KataGoEngine& engine, std::istringstream& args) {
  std::string colorStr;
  args >> colorStr;
  Player pla;
  try {
    pla = parseColor(colorStr);
  } catch(...) {
    return gtpError("invalid color");
  }
  std::string move = engine.generateMove(pla);
  if(move.empty())
    return gtpError("failed to generate move");
  return gtpSuccess(move);
}

std::string handleUndo(KataGoEngine& engine, std::istringstream&) {
  if(!engine.undoMove())
    return gtpError("cannot undo");
  return gtpSuccess();
}

std::string handleShowboard(KataGoEngine& engine, std::istringstream&) {
  return gtpSuccess("\n" + engine.showBoard());
}

std::string handleName(KataGoEngine&, std::istringstream&) {
  return gtpSuccess("KataGo");
}

std::string handleVersion(KataGoEngine&, std::istringstream&) {
  return gtpSuccess(Version::getKataGoVersion());
}

std::string handleProtocolVersion(KataGoEngine&, std::istringstream&) {
  return gtpSuccess("2");
}

std::string handleSetRules(KataGoEngine& engine, std::istringstream& args) {
  std::string rulesStr;
  args >> rulesStr;
  try {
    Rules r = Rules::parseRules(rulesStr);
    engine.setRules(r);
    return gtpSuccess();
  } catch(const std::exception& e) {
    return gtpError(std::string("unknown rules: ") + e.what());
  }
}

std::string handleAnalyze(KataGoEngine& engine, std::istringstream&) {
  return gtpSuccess(engine.analyze());
}

std::string handleQuit(KataGoEngine&, std::istringstream&) {
  return gtpSuccess();
}

std::string handleListCommands(KataGoEngine&, std::istringstream&);
std::string handleKnownCommand(KataGoEngine&, std::istringstream&);

// Forward declaration of registry accessor
const std::unordered_map<std::string, Handler>& commandRegistry();

std::string handleKnownCommand(KataGoEngine&, std::istringstream& args) {
  std::string cmd;
  args >> cmd;
  const auto& registry = commandRegistry();
  return gtpSuccess(registry.find(cmd) != registry.end() ? "true" : "false");
}

std::string handleListCommands(KataGoEngine&, std::istringstream&) {
  return gtpSuccess(
    "boardsize\nclear_board\nkomi\nplay\ngenmove\nundo\n"
    "showboard\nname\nversion\nprotocol_version\n"
    "list_commands\nknown_command\nkata-set-rules\n"
    "kata-analyze\nlz-analyze\nanalyze\nquit"
  );
}

// Command registry — add new commands here without touching dispatch().
const std::unordered_map<std::string, Handler>& commandRegistry() {
  static const std::unordered_map<std::string, Handler> registry = {
    {"boardsize",        handleBoardSize},
    {"clear_board",      handleClearBoard},
    {"komi",             handleKomi},
    {"play",             handlePlay},
    {"genmove",          handleGenmove},
    {"undo",             handleUndo},
    {"showboard",        handleShowboard},
    {"name",             handleName},
    {"version",          handleVersion},
    {"protocol_version", handleProtocolVersion},
    {"list_commands",    handleListCommands},
    {"known_command",    handleKnownCommand},
    {"kata-set-rules",   handleSetRules},
    {"kata-analyze",     handleAnalyze},
    {"lz-analyze",       handleAnalyze},
    {"analyze",          handleAnalyze},
    {"quit",             handleQuit},
  };
  return registry;
}

} // anonymous namespace

namespace GTPHandler {

std::string dispatch(KataGoEngine& engine, const std::string& commandLine) {
  std::istringstream in(commandLine);
  std::string token;
  in >> token;

  if(token.empty())
    return gtpError("empty command");

  const auto& registry = commandRegistry();
  auto it = registry.find(token);
  if(it == registry.end())
    return gtpError("unknown command");

  try {
    return it->second(engine, in);
  } catch(const std::exception& e) {
    return gtpError(e.what());
  }
}

} // namespace GTPHandler
