/**
 * @file overlay_state.hpp
 * @brief Singleton holding the overlay's global state (init status, version,
 * dirty flag).
 *
 * Replaces the old global variables g_initState, g_version, g_dirty with
 * a typesafe singleton. Thread safety is NOT guaranteed — the overlay
 * runs on a single thread.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */
#pragma once

#include <cstdio>
#include <cstring>

/**
 * @brief Process-wide singleton holding the overlay's init status, the
 *        sysmodule version string and the unsaved-changes flag.
 *
 * Not thread-safe: only the overlay UI thread is expected to touch it.
 */
class OverlayState {
public:
    /// Result of the overlay's initial connection to the sysmodule.
    enum class InitStatus { Uninit, Error, Loaded };

    /**
     * @brief Access the singleton (function-local static, lazily built).
     * @return Reference to the single OverlayState instance.
     */
    static OverlayState& Instance() {
        static OverlayState s_instance;
        return s_instance;
    }

    /**
     * @brief Current init status.
     * @return InitStatus::Uninit until SetStatus() is called.
     */
    InitStatus GetStatus() const {
        return m_status;
    }
    /**
     * @brief Record the init status.
     * @param s New status.
     */
    void SetStatus(InitStatus s) {
        m_status = s;
    }

    /**
     * @brief Sysmodule version string shown in the overlay.
     * @return NUL-terminated string owned by this object ("Unknown" until
     *         SetVersion() is called).
     */
    const char* GetVersion() const {
        return m_version;
    }
    /**
     * @brief Store the version string (truncated to the 32-byte buffer).
     * @param v NUL-terminated version; nullptr stores "Unknown".
     */
    void SetVersion(const char* v) {
        snprintf(m_version, sizeof(m_version), "%s", v ? v : "Unknown");
    }

    /**
     * @brief Whether settings were modified and not yet saved.
     * @return true between MarkDirty() and MarkSaved().
     */
    bool IsDirty() const {
        return m_dirty;
    }
    /// Flag the settings as modified but not yet saved.
    void MarkDirty() {
        m_dirty = true;
    }
    /// Clear the unsaved-changes flag after a successful save.
    void MarkSaved() {
        m_dirty = false;
    }
    /** Acquire an update lock. Call when entering an editor that modifies
     * values locally. */
    void AcquireUpdateLock() {
        m_updateLockCount++;
    }
    /** Release an update lock. Call when leaving the editor (apply, back,
     * cancel). */
    void ReleaseUpdateLock() {
        if (m_updateLockCount > 0)
            m_updateLockCount--;
    }
    /**
     * @brief Whether update locks are active.
     * @return true while at least one lock is held, in which case update()
     *         functions must skip IPC sync.
     */
    bool IsUpdateLocked() const {
        return m_updateLockCount > 0;
    }

private:
    OverlayState() = default;
    /// Current init status.
    InitStatus m_status = InitStatus::Uninit;
    /** Counter of active update locks. update() functions should skip IPC sync
     * when > 0. */
    u8 m_updateLockCount = 0;
    /// Version string, always NUL-terminated.
    char m_version[32] = "Unknown";
    /// True while there are unsaved setting changes.
    bool m_dirty = false;
};
