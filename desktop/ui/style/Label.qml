// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.Label {
    color: enabled ? Theme.textPrimary : Theme.textDisabled
    linkColor: Theme.accent
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody
}
