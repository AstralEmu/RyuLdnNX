/**
 * @file advanced_settings_gui.hpp
 * @brief Advanced settings overlay screen (server, LDN, debug).
 *
 * Shows settings removed from the main dashboard:
 * - Server address display (config file only)
 * - LDN enabled toggle
 * - Debug enabled toggle + debug level bar
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */
#pragma once

#include <tesla.hpp>

/** @brief Tesla screen exposing server address, P2P, and debug settings. */
class AdvancedSettingsGui : public tsl::Gui {
public:
    AdvancedSettingsGui();
    /**
     * @brief Builds the frame; shows a status line if not Loaded.
     * @return Root element of the screen (owned by Tesla).
     */
    virtual tsl::elm::Element* createUI() override;
    /** @brief Per-frame tick; refreshes the server address each second. */
    virtual void update() override;
    /**
     * @brief Handles input; the R button refreshes the server address at once.
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
    ~AdvancedSettingsGui();

private:
    /**
     * @brief Adds the read-only server address row (m_serverItem).
     * @param list List that receives the row.
     */
    void BuildServerSection(tsl::elm::List* list);
    /**
     * @brief Adds the P2P toggle (inverted disable_p2p setting) via IPC.
     * @param list List that receives the toggle.
     */
    void BuildLdnSection(tsl::elm::List* list);
    /**
     * @brief Adds the debug toggle and debug level bar, backed by IPC.
     * @param list List that receives both items.
     */
    void BuildDebugSection(tsl::elm::List* list);
    /** @brief Reads host:port over IPC and colors it by connection state. */
    void RefreshServerAddress();

    /// Server address row, set by BuildServerSection.
    tsl::elm::MiniListItem* m_serverItem = nullptr;
    /// P2P toggle, set by BuildLdnSection.
    tsl::elm::ToggleListItem* m_p2pToggle = nullptr;
    /// Debug toggle, set by BuildDebugSection.
    tsl::elm::ToggleListItem* m_debugToggle = nullptr;
    /// Debug level bar (0..3), set by BuildDebugSection.
    tsl::elm::NamedStepTrackBar* m_debugLevelBar = nullptr;
    /// Frames since the last periodic server address refresh.
    u32 m_refreshCounter = 0;
};
