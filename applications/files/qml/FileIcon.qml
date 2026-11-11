// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import org.shadow.ui

// A small file-type or application icon.
//
// Icon themes ship simplified, single-colour drawings for sizes below 32 px
// that are designed for one background colour. Small icons are therefore
// rendered from the full-colour 64 px drawing and scaled down, so list rows
// use the same artwork as the grid.
NxAppIcon {
    sourceSize: Qt.size(Math.round(Math.max(64, size) * Screen.devicePixelRatio),
                        Math.round(Math.max(64, size) * Screen.devicePixelRatio))
    mipmap: true
    // Inside a layout the implicit size would be that of the source image.
    Layout.preferredWidth: size
    Layout.preferredHeight: size
}
