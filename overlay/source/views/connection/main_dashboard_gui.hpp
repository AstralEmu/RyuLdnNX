/**
 * @file main_dashboard_gui.hpp
 * @brief Main dashboard overlay screen (connection + passphrase + nav).
 *
 * Displays:
 * - Connection section: status, LDN state, session info
 * - Passphrase section: display + edit button
 * - Navigation: advanced settings button
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */
#pragma once

#include <tesla.hpp>

#include "app/auto_save_controller.hpp"

/**
 * @brief Tesla screen showing connection state, LDN toggle and passphrase.
 *
 * All values come from the sysmodule over the ryu:cfg IPC service; the
 * widgets are owned by Tesla once added to the frame.
 */
class MainDashboardGui : public tsl::Gui {
public:
    /// Creates the screen; the widgets are built later by createUI().
    MainDashboardGui();

    /**
     * @brief Builds the frame; shows a status line if the IPC is not Loaded.
     * @return Root element of the screen (owned by Tesla).
     */
    virtual tsl::elm::Element* createUI() override;

    /**
     * @brief Per-frame tick; refreshes the values once every 60 frames.
     *
     * Also drives the auto-save controller and rebuilds the frame subtitle
     * (connection dot, save indicator, session info) and the passphrase
     * toggle lock state on every call.
     */
    virtual void update() override;

    /**
     * @brief Handles input: R refreshes, X randomizes and Y clears the key.
     * @param keysDown Buttons pressed this frame.
     * @param keysHeld Buttons held (unused).
     * @param touchPos Touch state (unused).
     * @param joyStickPosLeft Left stick (unused).
     * @param joyStickPosRight Right stick (unused).
     * @return true if the input was consumed.
     */
    virtual bool handleInput(
        u64 keysDown,
        u64 keysHeld,
        const HidTouchState& touchPos,
        HidAnalogStickState joyStickPosLeft,
        HidAnalogStickState joyStickPosRight) override;

private:
    /**
     * @brief Adds the Connection header, status row and passphrase row.
     * @param list List that receives the items.
     */
    void BuildConnectionSection(tsl::elm::List* list);

    /**
     * @brief Adds the passphrase toggle and the edit button.
     * @param list List that receives the items.
     */
    void BuildPassphraseSection(tsl::elm::List* list);

    /**
     * @brief Adds the LDN enabled toggle, initialized over IPC.
     * @param list List that receives the toggle.
     */
    void BuildLdnSection(tsl::elm::List* list);

    /**
     * @brief Adds the button that opens the advanced settings screen.
     * @param list List that receives the button.
     */
    void BuildNavSection(tsl::elm::List* list);

    /// Updates the status row and the cached "n/max" session subtitle.
    void RefreshConnectionValues();

    /// Re-reads the passphrase over IPC and updates its row and color.
    void RefreshPassphrase();

    /// Root frame, set by createUI(); used to update the subtitle.
    tsl::elm::OverlayFrame* m_frame = nullptr;

    /// Connection status row, set by BuildConnectionSection.
    tsl::elm::MiniListItem* m_statusItem = nullptr;
    /// Masked passphrase row, set by BuildConnectionSection.
    tsl::elm::MiniListItem* m_passphraseItem = nullptr;

    /// LDN enabled toggle, set by BuildLdnSection.
    tsl::elm::ToggleListItem* m_ldnToggle = nullptr;
    /// Use-passphrase toggle (locked while no passphrase is set).
    tsl::elm::ToggleListItem* m_passphraseEnabledToggle = nullptr;

    /// Frames since the last periodic refresh (refresh at 60).
    u32 m_refreshCounter = 0;
    /// Debounced saver that persists config changes marked dirty.
    AutoSaveController m_autoSaveController;

    /** @brief Cached info line built in RefreshConnectionValues, used in
     * update() subtitle. */
    std::string m_cachedInfoSubtitle;
};
