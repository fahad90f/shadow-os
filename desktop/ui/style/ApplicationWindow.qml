// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.ApplicationWindow {
    id: window
    color: Theme.bgBase
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    // Standard palette roles for anything that still reads them.
    palette.window: Theme.bgBase
    palette.windowText: Theme.textPrimary
    palette.base: Theme.bgInput
    palette.alternateBase: Theme.bgRaised
    palette.text: Theme.textPrimary
    palette.button: Theme.bgRaised
    palette.buttonText: Theme.textPrimary
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.textOnAccent
    palette.placeholderText: Theme.textMuted
    palette.toolTipBase: Theme.bgOverlay
    palette.toolTipText: Theme.textPrimary
    palette.mid: Theme.border
    palette.dark: Theme.borderStrong
    palette.light: Theme.bgHover
    palette.link: Theme.accent
}
