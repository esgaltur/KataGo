/**
 * katago_gtp_handler.h - GTP command dispatcher (Command pattern)
 *
 * Parses a GTP command string, dispatches to the appropriate KataGoEngine
 * method, and formats the GTP response. Each command is a small handler
 * registered in a lookup table — easy to extend without modifying existing code
 * (Open/Closed Principle).
 *
 * This is an internal header — not part of the public C API.
 */

#ifndef KATAGO_GTP_HANDLER_H
#define KATAGO_GTP_HANDLER_H

#include <string>

struct KataGoEngine;

namespace GTPHandler {

// Dispatch a single GTP command string against the given engine.
// Returns the full GTP response (e.g. "= D4\n" or "? illegal move\n").
std::string dispatch(KataGoEngine& engine, const std::string& commandLine);

}

#endif /* KATAGO_GTP_HANDLER_H */
