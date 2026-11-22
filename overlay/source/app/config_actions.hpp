/**
 * @file config_actions.hpp
 * @brief Standalone config save/reload helper functions.
 *
 * Extracted from auto_save_controller.hpp to avoid dependency on the
 * AutoSaveController class. Used by AdvancedSettingsGui.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */
#pragma once

#include <switch.h>

#include "ryu_ldn_ipc.h"
#include "app/overlay_state.hpp"

/**
 * @brief Persist the sysmodule config to the SD card through IPC.
 *
 * Does nothing unless the overlay state is Loaded. On success the overlay
 * dirty flag is cleared with OverlayState::MarkSaved().
 *
 * @return true if the sysmodule reported ConfigResult Success.
 */
inline bool DoSaveConfig() {
    if (OverlayState::Instance().GetStatus() !=
        OverlayState::InitStatus::Loaded)
        return false;
    RyuLdnConfigService* svc = ryuLdnGetService();
    if (!svc)
        return false;
    RyuLdnConfigResult result;
    Result rc = ryuLdnSaveConfig(svc, &result);
    if (R_SUCCEEDED(rc) && result == RyuLdnConfigResult_Success) {
        OverlayState::Instance().MarkSaved();
        return true;
    }
    return false;
}

/**
 * @brief Reload the sysmodule config from the SD card through IPC.
 *
 * Does nothing unless the overlay state is Loaded. In-memory sysmodule
 * changes are discarded; the overlay dirty flag is not touched here.
 *
 * @return true if the sysmodule reported ConfigResult Success.
 */
inline bool DoReloadConfig() {
    if (OverlayState::Instance().GetStatus() !=
        OverlayState::InitStatus::Loaded)
        return false;
    RyuLdnConfigService* svc = ryuLdnGetService();
    if (!svc)
        return false;
    RyuLdnConfigResult result;
    Result rc = ryuLdnReloadConfig(svc, &result);
    return R_SUCCEEDED(rc) && result == RyuLdnConfigResult_Success;
}
