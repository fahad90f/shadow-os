// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Rectangle {
    property bool vertical: false
    implicitWidth: vertical ? 1 : 100
    implicitHeight: vertical ? 100 : 1
    color: Theme.borderSubtle
}
