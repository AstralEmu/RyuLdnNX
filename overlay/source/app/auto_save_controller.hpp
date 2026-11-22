/**
 * @file auto_save_controller.hpp
 * @brief Auto-save logic extracted from the main dashboard.
 *
 * Manages two counters:
 * - m_autoSaveCounter: counts frames while dirty, triggers save after
 *   AutoSaveDelayFrames (60 frames ~ 1 s at 60 fps).
 * - m_saveConfirmCounter: counts frames after successful save to show
 *   "Saved!" indicator for ConfirmDurationFrames (120 frames ~ 2 s).
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */
#pragma once

#include <string>

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

/**
 * @brief Frame-driven auto-save of the sysmodule configuration.
 *
 * Driven from the UI thread through Update(); holds only two frame counters
 * and no other state.
 */
class AutoSaveController {
public:
    /**
     * @brief Call once per frame. Marks OverlayState as saved if auto-save
     * fires.
     *
     * While dirty and no LDN session is active, counts frames and saves after
     * AutoSaveDelayFrames. An active LDN session or a clean state resets the
     * counter. Also counts the "Saved!" indicator down.
     *
     * @param isDirty true when the overlay has unsaved config changes.
     */
    void Update(bool isDirty) {
        if (isDirty) {
            // Skip auto-save if LDN session is active (prevents memory bugs)
            if (!IsLdnSessionActive()) {
                m_autoSaveCounter++;
                if (m_autoSaveCounter >= AutoSaveDelayFrames) {
                    m_autoSaveCounter = 0;
                    if (DoSaveConfig()) {
                        m_saveConfirmCounter = ConfirmDurationFrames;
                    }
                }
            } else {
                m_autoSaveCounter = 0;
            }
        } else {
            m_autoSaveCounter = 0;
        }
        if (m_saveConfirmCounter > 0) {
            m_saveConfirmCounter--;
        }
    }

    /** @brief Force an immediate save attempt. */
    void ForceSave() {
        m_autoSaveCounter = 0;
        if (DoSaveConfig()) {
            m_saveConfirmCounter = ConfirmDurationFrames;
        }
    }

    /** @brief Reset confirm counter (e.g. after reload). */
    void ResetConfirm() {
        m_saveConfirmCounter = 0;
    }

    /**
     * @brief Whether the "Saved!" flash indicator should be visible.
     * @return true while the confirm counter has frames left.
     */
    bool IsConfirmVisible() const {
        return m_saveConfirmCounter > 0;
    }

    /**
     * @brief Get the text for the config dirty indicator.
     * @return "Saved!" during the confirm flash, else "Saved" or an
     *         "Unsaved changes" warning from the overlay dirty flag.
     */
    std::string GetIndicatorText() const {
        bool isDirty = OverlayState::Instance().IsDirty();
        if (m_saveConfirmCounter > 0) {
            return "Saved!";
        }
        return isDirty ? "\u26A0 Unsaved changes" : "Saved";
    }

private:
    /**
     * @brief Check if an LDN session is currently active.
     * @return true if ryu:cfg reports a game using LDN; false when the
     *         service is unavailable or the IPC call fails.
     */
    bool IsLdnSessionActive() const {
        RyuLdnConfigService* svc = ryuLdnGetService();
        if (!svc)
            return false;
        u32 gameActive;
        if (R_FAILED(ryuLdnIsGameActive(svc, &gameActive)))
            return false;
        return gameActive != 0;
    }
    /// Frames elapsed while dirty with no LDN session; save fires at the delay.
    u32 m_autoSaveCounter = 0;
    /// Frames left during which the "Saved!" indicator stays visible.
    u32 m_saveConfirmCounter = 0;
    /// Dirty frames to wait before auto-saving (60 frames, 1 s at 60 fps).
    static constexpr u32 AutoSaveDelayFrames = 60;
    /// Frames the "Saved!" indicator stays visible (120 frames, 2 s).
    static constexpr u32 ConfirmDurationFrames = 120;
};
