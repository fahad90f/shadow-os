// SPDX-License-Identifier: GPL-3.0-or-later
// Single-line input for the login screen, with an optional reveal toggle for
// secrets.
import QtQuick 2.15
import "../tokens.js" as T

FocusScope {
    id: field

    property real u: 1
    property alias text: input.text
    property string placeholder: ""
    property bool secret: false
    property bool revealed: false
    property bool error: false
    // The border changes colour over 140 ms when the keyboard focus comes or
    // goes. Off until the screen has built up, so the focus ring appears at
    // once when the window gets the keyboard. An error turns the border red
    // at once.
    property bool animateFocus: false
    // The ring means exactly "keys arrive here": it is drawn only while the
    // field really has the keyboard, which needs the window to have it too.
    // It is never drawn in advance, not even during the entrance: with two
    // monitors the window may not get the keyboard at all.
    readonly property bool ringed: input.activeFocus
    property color focusColor: ringed ? T.accent : T.borderStrong
    Behavior on focusColor {
        enabled: field.animateFocus
        ColorAnimation { duration: T.motionFast }
    }
    property string showText: "Show password"
    property string hideText: "Hide password"
    readonly property alias input: input
    signal accepted()
    // A letter was typed: upper tells whether it arrived as a capital,
    // shift whether Shift was held (Caps Lock detection of last resort).
    signal letterTyped(bool upper, bool shift)

    implicitHeight: Math.round(T.controlHeight * u)
    implicitWidth: Math.round(280 * u)
    height: implicitHeight
    opacity: enabled ? 1 : 0.6

    Rectangle {
        anchors.fill: parent
        radius: Math.round(T.radiusMd * field.u)
        color: T.glassField
        border.width: field.ringed ? Math.max(2, Math.round(T.focusRing * field.u)) : 1
        border.color: field.error ? T.danger : field.focusColor
    }

    TextInput {
        id: input
        anchors.left: parent.left
        anchors.right: reveal.visible ? reveal.left : parent.right
        anchors.leftMargin: Math.round(T.spaceMd * field.u)
        anchors.rightMargin: reveal.visible ? Math.round(T.spaceXs * field.u) : Math.round(T.spaceMd * field.u)
        anchors.verticalCenter: parent.verticalCenter
        focus: true
        activeFocusOnTab: true
        clip: true
        color: T.textPrimary
        selectionColor: T.accent
        selectedTextColor: T.textOnAccent
        font.family: T.fontUi
        font.pixelSize: Math.round(T.fontBodyLarge * field.u)
        echoMode: field.secret && !field.revealed ? TextInput.Password : TextInput.Normal
        passwordCharacter: "•"
        // Secrets are neither predicted nor learned by an input method.
        inputMethodHints: field.secret ? (Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase)
                                       : (Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase)
        selectByMouse: true
        Accessible.name: field.placeholder
        Accessible.passwordEdit: field.secret && !field.revealed
        onAccepted: field.accepted()
        Keys.onPressed: function (event) {
            var t = event.text
            if (t.length === 1 && t.toLowerCase() !== t.toUpperCase())
                field.letterTyped(t === t.toUpperCase(), (event.modifiers & Qt.ShiftModifier) !== 0)
        }
    }

    Text {
        anchors.left: input.left
        anchors.right: input.right
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text === "" && input.preeditText === ""
        text: field.placeholder
        elide: Text.ElideRight
        color: T.textMuted
        font: input.font
    }

    NxButton {
        id: reveal
        u: field.u
        visible: field.secret
        anchors.right: parent.right
        anchors.rightMargin: Math.round(3 * field.u)
        anchors.verticalCenter: parent.verticalCenter
        implicitHeight: field.height - Math.round(6 * field.u)
        implicitWidth: implicitHeight
        iconSize: 18 * field.u
        icon: field.revealed ? "eye-off" : "eye"
        description: field.revealed ? field.hideText : field.showText
        onClicked: {
            field.revealed = !field.revealed
            input.forceActiveFocus()
        }
    }
}
