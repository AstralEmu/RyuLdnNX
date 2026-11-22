/**
 * @file passphrase_editor_gui.hpp
 * @brief LDN passphrase editor overlay screen - compact horizontal layout.
 *
 * Displays 8 hex characters on a single row with a cursor indicator.
 * ←/→ moves the cursor between positions, ↑/↓ increments/decrements
 * the hex value at the current cursor position (0-9 a-f, wrapping).
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */
#pragma once

#include <tesla.hpp>

/**
 * @brief Overlay screen editing the 8 hex characters of the LDN passphrase.
 *
 * The passphrase is "Ryujinx-" followed by the 8 edited hex characters.
 * Runs on the Tesla overlay UI thread.
 */
class PassphraseEditorGui : public tsl::Gui {
public:
    /** Load the current passphrase from the sysmodule and take the update
     * lock so live sync does not overwrite edits. */
    PassphraseEditorGui();
    /** Build the frame with the preview, character and caret lines.
     * @return Newly allocated root element (owned by Tesla). */
    virtual tsl::elm::Element* createUI() override;
    /** Every 60 frames, re-read the passphrase from the sysmodule unless an
     * editor holds the update lock. */
    virtual void update() override;
    /**
     * @brief Handle navigation and edit keys.
     *
     * Left/Right move the cursor (wrapping), Up/Down change the hex digit
     * (wrapping), B or Plus apply the passphrase and go back.
     *
     * @param keysDown Buttons pressed this frame.
     * @param keysHeld Buttons currently held (unused).
     * @param touchPos Touch state (unused).
     * @param joyStickPosLeft Left stick state (unused).
     * @param joyStickPosRight Right stick state (unused).
     * @return true when the input was consumed, false otherwise.
     */
    virtual bool handleInput(
        u64 keysDown,
        u64 keysHeld,
        const HidTouchState& touchPos,
        HidAnalogStickState joyStickPosLeft,
        HidAnalogStickState joyStickPosRight) override;
    /** Release the update lock taken by the constructor. */
    ~PassphraseEditorGui();

private:
    /** Hex value (0-15) for each of the 8 characters. */
    u8 m_charIndices[8] = {0};
    /** Currently selected cursor position (0-7). */
    u8 m_cursorPos = 0;
    /** Frame counter for periodic refresh. */
    u32 m_updateCounter = 0;

    /** Preview line showing the hex passphrase value. */
    tsl::elm::MiniListItem* m_previewItem = nullptr;
    /** Display line showing the 8 hex chars with cursor highlighted. */
    tsl::elm::ListItem* m_charsItem = nullptr;
    /** Cursor indicator line showing spaces and a caret at the selected
     * position. */
    tsl::elm::MiniListItem* m_cursorItem = nullptr;

    /** Lookup table mapping a hex value (0-15) to its lowercase digit. */
    static constexpr const char* HEX_CHARS = "0123456789abcdef";

    /** Build the hex string from m_charIndices.
     * @return The 8 hex digits. */
    std::string BuildHexString();
    /** Build the display string with brackets around the selected char.
     * @return The 8 digits separated by spaces, e.g. "[a] 3 f 7 9 b 2 c". */
    std::string BuildCursorDisplay();
    /** Build the cursor indicator string with spaces and a caret.
     * @return 3 spaces per position before the cursor, then " ^ ". */
    std::string BuildCursorIndicator();
    /** Refresh all UI elements to reflect current state. */
    void RefreshDisplay();
    /** Apply the passphrase and go back. */
    void ApplyPassphrase();
};
