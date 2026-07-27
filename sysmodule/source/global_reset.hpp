/**
 * @file global_reset.hpp
 * @brief Global state reset between game sessions
 *
 * Declares ResetGlobalState() in the neutral ams::mitm namespace so that
 * both the BSD MITM and the LDN communication service can call it without
 * creating a circular include dependency (LDN -> BSD -> LDN).
 *
 * The implementation lives in bsd_mitm_service.cpp.
 */
#pragma once

namespace ams::mitm {

/**
 * @brief Reset all cross-module global state for a new game session.
 *
 * Resets ProxySocketManager, global socket/PID tables, abandoned
 * forward services, and LDN SharedState. Gated by an atomic flag and
 * a TOCTOU re-check of the active session counter so that a session
 * created between the decrement and the reset is not wiped.
 *
 * Safe to call from either the BSD or LDN destructor path.
 */
void ResetGlobalState();

} // namespace ams::mitm