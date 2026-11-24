// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import org.shadow.ui

// One numeric setting: a row with a title, an explanation and a spin box that
// handles decimals (SpinBox itself counts in whole numbers).
NxRow {
    id: root

    property real value: 0
    property real from: 0
    property real to: 100
    property real step: 1
    property int decimals: 0
    property string unit: ""
    signal edited(real value)

    readonly property real scale: Math.pow(10, decimals)

    SpinBox {
        id: box
        Accessible.name: root.title
        editable: true
        from: Math.round(root.from * root.scale)
        to: Math.round(root.to * root.scale)
        stepSize: Math.max(1, Math.round(root.step * root.scale))
        value: Math.round(root.value * root.scale)
        textFromValue: (v, locale) => Number(v / root.scale).toLocaleString(locale, "f", root.decimals)
            + (root.unit !== "" ? " " + root.unit : "")
        valueFromText: (text, locale) => {
            var n = Number.fromLocaleString(locale, text.replace(root.unit, "").trim())
            return isNaN(n) ? box.value : Math.round(n * root.scale)
        }
        onValueModified: root.edited(value / root.scale)
    }
}
