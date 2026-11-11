// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// Text that comes from outside the application (package names, summaries,
// error messages, repository data). It is always rendered as plain text: Qt's
// default "auto" format would interpret anything that looks like HTML.
Text {
    textFormat: Text.PlainText
    color: Theme.textPrimary
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody
}
