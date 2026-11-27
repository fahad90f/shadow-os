// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHADOW lock screen, loaded by KScreenLocker's greeter (kscreenlocker_greet)
// as "lockscreenmainscript" of the shell package org.shadow.horizon.
//
// Deliberately plain: QtQuick only, no optional module, no shader, nothing
// that can fail to load — when this file does not load the greeter falls
// back to a built-in screen that needs Plasma modules SHADOW does not ship,
// and the user would be locked out.
//
// For the same reason everything decorative (scan line, ring, brackets) is
// one separate file, components/LockDecor.qml, behind a Loader: this file
// imports nothing new and reads one value from the decoration, "ent", the
// progress of the 0.4 s entrance. If the decoration does not load, ent is 1
// and the form below is complete and unlocks as before. The password field
// exists, is enabled and focused from the first frame; the entrance only
// changes opacity. There is no exit animation: a right password ends the
// greeter at once. The setting reduceMotion (config.xml) switches every
// movement off.
//
// Provided by the greeter (kscreenlocker 6.3, greeter/greeterapp.cpp):
//   authenticator            PamAuthenticators: startAuthenticating(), respond(bytes), cancel(),
//                            state, busy, prompt, promptForSecret, infoMessage, errorMessage,
//                            signals succeeded(), failed(kind, authenticator)
//   kscreenlocker_userName   full name (or login name) of the session's user
//   config                   values of contents/lockscreen/config.xml
// and written to the root item: locked, viewVisible, suspendToRamSupported,
// suspendToDiskSupported. Qt.quit() ends the greeter; it is honoured only
// after authentication succeeded.
//
// Keyboard: typing goes to the password field, Return unlocks, Tab reaches
// the reveal and unlock buttons, Escape clears the field.
import QtQuick 2.15
import "tokens.js" as T
import "state.js" as S

Item {
    id: root

    // ---- interface of the greeter -----------------------------------------------
    property bool locked: false
    property bool viewVisible: false
    property bool suspendToRamSupported: false
    property bool suspendToDiskSupported: false

    onLockedChanged: if (locked) root.start()
    onViewVisibleChanged: {
        if (viewVisible) {
            field.forceActiveFocus()
            root.start()
            // The screen is shown (again): the entrance runs (again).
            if (decor.item)
                decor.item.restart()
        }
    }

    // ---- state ----------------------------------------------------------------------
    readonly property real u: Math.max(0.9, Math.min(3, Math.min(width / 1280, height / 800)))
    readonly property bool hasAuthenticator: typeof authenticator !== "undefined" && authenticator !== null
    readonly property bool busy: hasAuthenticator && authenticator.busy === true
    readonly property string userName: typeof kscreenlocker_userName !== "undefined" && kscreenlocker_userName
                                       ? String(kscreenlocker_userName) : ""
    readonly property string wallpaperPath: typeof config !== "undefined" && config && config.wallpaper
                                            ? String(config.wallpaper) : "/usr/share/backgrounds/shadow/shadow-horizon.jpg"
    readonly property string timeFormat: Qt.locale().timeFormat(Locale.ShortFormat).replace(/[:.]s+/, "")
    // No entrance and no shake. Written by shadow-apply-appearance from the
    // user's "Reduce motion" switch (kscreenlockerrc, see config.xml).
    readonly property bool reduceMotion: typeof config !== "undefined" && config && config.reduceMotion === true
    // Progress of the entrance, 0..1, counted by the decoration. Without the
    // decoration (the file did not load) it is 1: a complete screen.
    readonly property real ent: decor.status === Loader.Ready ? decor.item.t : 1
    property date now: new Date()
    property string errorText: ""
    property string infoText: ""
    property string promptText: ""        // PAM's own wording when it asks for something other than the password
    property bool echoPrompt: false       // PAM asked a question whose answer is not secret
    property bool capsLock: false         // guessed from typed letters
    property bool revealed: false

    function start() {
        // Starts the PAM conversation (and fingerprint/smart-card readers
        // when configured). No-op while one is running or during the grace
        // period.
        if (hasAuthenticator && locked)
            authenticator.startAuthenticating()
    }

    function submit() {
        if (!hasAuthenticator)
            return
        errorText = ""
        infoText = ""
        S.submit(authenticator, field.text, authenticator.state === 0)
    }

    // Share of the window from..to (milliseconds of the entrance) that has
    // passed, slowing down towards its end.
    function ph(from, to) {
        var x = 1 - Math.max(0, Math.min(1, (ent * T.motionEntranceLock - from) / (to - from)))
        return 1 - x * x * x
    }

    function initials(text) {
        var words = String(text).trim().split(/[\s._-]+/).filter(function (w) { return w.length > 0 })
        if (words.length === 0)
            return ""
        return (words[0].charAt(0) + (words.length > 1 ? words[words.length - 1].charAt(0) : "")).toUpperCase()
    }

    Connections {
        target: root.hasAuthenticator ? authenticator : null
        ignoreUnknownSignals: true

        function onSucceeded() {
            Qt.quit()
        }
        function onFailed(kind) {
            // Fingerprint and smart-card services report here as well
            // (kind != 0); only the password conversation concerns the form.
            // An unreadable kind counts as the password conversation: never
            // leave a wrong password without feedback.
            if (Number(kind) > 0)
                return
            S.reset()
            field.text = ""
            root.revealed = false
            root.promptText = ""
            root.echoPrompt = false
            root.errorText = qsTr("Wrong password. Try again.")
            field.forceActiveFocus()
            if (!root.reduceMotion)
                shake.restart()
            if (decor.item)
                decor.item.fail()
            restart.restart()
        }
        function onPromptForSecretChanged() {
            var text = String(authenticator.promptForSecret).trim()
            // "Password:" is the ordinary question; anything else (a token
            // code, a new password) is shown in PAM's words.
            root.promptText = /^password:?$/i.test(text) ? "" : text
            root.echoPrompt = false
            S.prompted(authenticator)
            Qt.callLater(S.promptDelivered)
        }
        function onPromptChanged() {
            root.promptText = String(authenticator.prompt).trim()
            root.echoPrompt = true
            S.prompted(authenticator)
            Qt.callLater(S.promptDelivered)
        }
        function onInfoMessageChanged() {
            root.infoText = String(authenticator.infoMessage)
        }
        function onErrorMessageChanged() {
            root.errorText = String(authenticator.errorMessage)
        }
    }

    // After a failure PAM is asked again, so the next attempt (and the
    // fingerprint reader) is ready without another key press.
    Timer {
        id: restart
        interval: 150
        onTriggered: root.start()
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: root.now = new Date()
    }

    // ---- background -----------------------------------------------------------------
    // The near-black field of every start-up surface: the first frame, and
    // what stays when there is no picture.
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: T.fieldTop }
            GradientStop { position: 1.0; color: T.fieldBottom }
        }
    }
    // The scan line of the entrance resolves the picture from the top: the
    // picture sits in a clip whose lower edge is the line (0–260 ms).
    Item {
        width: root.width
        height: Math.round(root.height * Math.min(1, root.ent * T.motionEntranceLock / T.motionSweepLock))
        clip: true

        Item {
            width: root.width
            height: root.height
            // A picture that is decoded only after the entrance has started
            // fades in when it arrives.
            opacity: wallpaper.status === Image.Ready ? 1 : 0
            Behavior on opacity {
                enabled: root.ent > 0 && !root.reduceMotion
                NumberAnimation { duration: 300 }
            }

            Image {
                id: wallpaper
                anchors.fill: parent
                source: root.wallpaperPath !== "" ? "file://" + root.wallpaperPath : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: false
                sourceSize.width: Math.max(1, root.width)
                sourceSize.height: Math.max(1, root.height)
                visible: status === Image.Ready
            }
            Rectangle {
                anchors.fill: parent
                visible: wallpaper.visible
                gradient: Gradient {
                    GradientStop { position: 0.0; color: T.scrimTop }
                    GradientStop { position: 1.0; color: T.scrimBottom }
                }
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        onClicked: field.forceActiveFocus()
    }

    // ---- decoration and the clock of the entrance ----------------------------------------
    // Optional by construction: the only thing in this file that can fail to
    // load, and nothing below depends on it (see the header).
    Loader {
        id: decor
        anchors.fill: parent
        source: "components/LockDecor.qml"
    }
    Binding { target: decor.item; property: "u"; value: root.u; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "reduce"; value: root.reduceMotion; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "now"; value: root.now; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "busy"; value: root.busy; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "focused"; value: fieldBox.ringed; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "caps"; value: root.capsLock; when: decor.status === Loader.Ready }
    // Where the ring and the brackets belong: around the user picture and
    // around the password row, wherever the card is (it moves when it shakes).
    Binding {
        target: decor.item; property: "ringCenter"; when: decor.status === Loader.Ready
        value: Qt.point(unlockArea.x + card.x + ringSlot.x + ringSlot.width / 2,
                        unlockArea.y + card.y + ringSlot.y + ringSlot.height / 2)
    }
    Binding {
        target: decor.item; property: "fieldRect"; when: decor.status === Loader.Ready
        value: Qt.rect(unlockArea.x + card.x + passwordRow.x, unlockArea.y + card.y + passwordRow.y,
                       passwordRow.width, passwordRow.height)
    }

    // ---- clock ----------------------------------------------------------------------
    Column {
        id: clock
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(root.height * 0.085)
        spacing: Math.round(2 * root.u)

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatTime(root.now, root.timeFormat)
            color: T.textPrimary
            font.family: T.fontUi
            font.pixelSize: Math.round(76 * root.u)
            font.weight: Font.Light
            opacity: root.ph(0, 200)
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.now.toLocaleDateString(Qt.locale(), Locale.LongFormat)
            color: T.textSecondary
            font.family: T.fontUi
            font.pixelSize: Math.round(T.fontTitle * root.u)
            opacity: root.ph(40, 240)
        }
    }

    // ---- unlock ---------------------------------------------------------------------
    Item {
        id: unlockArea
        anchors.top: clock.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        Column {
            id: card
            width: Math.round(320 * root.u)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: shakeOffset
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -Math.round(root.height * 0.04)
            spacing: Math.round(T.spaceMd * root.u)
            property real shakeOffset: 0

            SequentialAnimation {
                id: shake
                NumberAnimation { target: card; property: "shakeOffset"; to: -10 * root.u; duration: 50 }
                NumberAnimation { target: card; property: "shakeOffset"; to: 10 * root.u; duration: 80 }
                NumberAnimation { target: card; property: "shakeOffset"; to: -6 * root.u; duration: 70 }
                NumberAnimation { target: card; property: "shakeOffset"; to: 0; duration: 60 }
            }

            // The user picture, with room around it for the ring that the
            // decoration draws (hexagon and tick ring, 152u across).
            Item {
                id: ringSlot
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.round(152 * root.u)
                height: width

                Rectangle {
                    id: avatar
                    anchors.centerIn: parent
                    width: Math.round(104 * root.u)
                    height: width
                    radius: width / 2
                    // Behind the scan line, which reaches the ring at
                    // about 90 ms.
                    opacity: root.ph(90, 210)
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: T.gradientStart }
                        GradientStop { position: 0.6; color: T.gradientMid }
                        GradientStop { position: 1.0; color: T.gradientEnd }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: text !== ""
                        text: root.initials(root.userName)
                        color: T.textOnAccent
                        font.family: T.fontUi
                        font.pixelSize: Math.round(avatar.width * 0.38)
                        font.weight: Font.DemiBold
                    }
                    Image {
                        anchors.centerIn: parent
                        visible: root.initials(root.userName) === ""
                        source: "icons/user-on-accent.svg"
                        sourceSize.width: Math.round(avatar.width * 0.5)
                        sourceSize.height: Math.round(avatar.width * 0.5)
                    }
                }
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: text !== ""
                text: root.userName
                elide: Text.ElideRight
                color: T.textPrimary
                font.family: T.fontUi
                font.pixelSize: Math.round(T.fontHeading * root.u)
                font.weight: Font.DemiBold
                opacity: root.ph(130, 300)
            }

            // The "Locked" line, with 6u more room below it than the
            // column spacing, so the brackets around the password row keep
            // clear of the text.
            Item {
                width: parent.width
                height: lockLine.height + Math.round(6 * root.u)

                Row {
                    id: lockLine
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Math.round(T.spaceSm * root.u)
                    opacity: root.ph(150, 320)
                    Image {
                        anchors.verticalCenter: parent.verticalCenter
                        source: "icons/lock.svg"
                        sourceSize.width: Math.round(14 * root.u)
                        sourceSize.height: Math.round(14 * root.u)
                        opacity: 0.7
                    }
                    Text {
                        text: root.promptText !== "" ? root.promptText : qsTr("Locked — enter your password to unlock")
                        color: T.textSecondary
                        font.family: T.fontUi
                        font.pixelSize: Math.round(T.fontBody * root.u)
                    }
                }
            }

            // Field and button exist, are enabled and focused from the first
            // frame; the entrance only raises them from 55 % to full opacity
            // (0–100 ms). They are never hidden, disabled or loaded later.
            Row {
                id: passwordRow
                width: parent.width
                spacing: Math.round(T.spaceSm * root.u)
                opacity: 0.55 + 0.45 * root.ph(0, 100)

                // Password field.
                Rectangle {
                    id: fieldBox
                    width: parent.width - unlock.width - parent.spacing
                    height: Math.round(T.controlHeight * root.u)
                    radius: Math.round(T.radiusMd * root.u)
                    color: T.glassField
                    // The ring means exactly "keys arrive here": it is drawn
                    // only while the field really has the keyboard (which
                    // needs the window to have it), never in advance.
                    readonly property bool ringed: field.activeFocus
                    // The ring changes colour over 140 ms when the focus comes
                    // or goes; an error turns the border red at once.
                    property color focusColor: ringed ? T.accent : T.borderStrong
                    Behavior on focusColor {
                        enabled: root.ent >= 1 && !root.reduceMotion
                        ColorAnimation { duration: T.motionFast }
                    }
                    border.width: ringed ? Math.max(2, Math.round(T.focusRing * root.u)) : 1
                    border.color: root.errorText !== "" ? T.danger : focusColor

                    TextInput {
                        id: field
                        anchors.left: parent.left
                        anchors.right: reveal.left
                        anchors.leftMargin: Math.round(T.spaceMd * root.u)
                        anchors.rightMargin: Math.round(T.spaceXs * root.u)
                        anchors.verticalCenter: parent.verticalCenter
                        focus: true
                        activeFocusOnTab: true
                        clip: true
                        // Typing stays possible while PAM checks; only the
                        // answer is held back (see state.js).
                        color: T.textPrimary
                        selectionColor: T.accent
                        selectedTextColor: T.textOnAccent
                        font.family: T.fontUi
                        font.pixelSize: Math.round(T.fontBodyLarge * root.u)
                        echoMode: root.revealed || root.echoPrompt ? TextInput.Normal : TextInput.Password
                        passwordCharacter: "•"
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                        Accessible.name: qsTr("Password")
                        Accessible.passwordEdit: echoMode === TextInput.Password
                        onAccepted: root.submit()
                        onTextChanged: if (text !== "") root.errorText = ""
                        Keys.onEscapePressed: { text = ""; root.revealed = false }
                        Keys.onPressed: function (event) {
                            var t = event.text
                            if (t.length === 1 && t.toLowerCase() !== t.toUpperCase())
                                root.capsLock = (t === t.toUpperCase()) !== ((event.modifiers & Qt.ShiftModifier) !== 0)
                        }
                    }
                    Text {
                        anchors.left: field.left
                        anchors.right: field.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: field.text === ""
                        text: root.busy ? qsTr("Checking…") : qsTr("Password")
                        elide: Text.ElideRight
                        color: T.textMuted
                        font: field.font
                    }

                    // Show / hide the typed password.
                    Item {
                        id: reveal
                        anchors.right: parent.right
                        anchors.rightMargin: Math.round(3 * root.u)
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.height - Math.round(6 * root.u)
                        height: width
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: root.revealed ? qsTr("Hide password") : qsTr("Show password")
                        function toggle() { root.revealed = !root.revealed; field.forceActiveFocus() }
                        Keys.onReturnPressed: toggle()
                        Keys.onEnterPressed: toggle()
                        Keys.onSpacePressed: toggle()

                        Rectangle {
                            anchors.fill: parent
                            radius: Math.round(T.radiusSm * root.u)
                            color: revealMouse.pressed ? T.pressedFill : revealMouse.containsMouse ? T.hoverFill : "transparent"
                            border.width: reveal.activeFocus ? Math.max(2, Math.round(T.focusRing * root.u)) : 0
                            border.color: T.accent
                        }
                        Image {
                            anchors.centerIn: parent
                            source: root.revealed ? "icons/eye-off.svg" : "icons/eye.svg"
                            sourceSize.width: Math.round(18 * root.u)
                            sourceSize.height: Math.round(18 * root.u)
                        }
                        MouseArea {
                            id: revealMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: reveal.toggle()
                        }
                    }
                }

                // Unlock.
                Item {
                    id: unlock
                    width: Math.round(T.controlHeight * root.u)
                    height: width
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Unlock")
                    Keys.onReturnPressed: root.submit()
                    Keys.onEnterPressed: root.submit()
                    Keys.onSpacePressed: root.submit()

                    Rectangle {
                        anchors.fill: parent
                        radius: Math.round(T.radiusMd * root.u)
                        color: unlockMouse.pressed ? Qt.darker(T.accent, 1.25) : unlockMouse.containsMouse ? Qt.lighter(T.accent, 1.12) : T.accent
                        opacity: root.busy ? 0.5 : 1
                        border.width: unlock.activeFocus ? Math.max(2, Math.round(T.focusRing * root.u)) : 0
                        border.color: T.textPrimary
                    }
                    Image {
                        anchors.centerIn: parent
                        source: "icons/arrow-right-on-accent.svg"
                        sourceSize.width: Math.round(20 * root.u)
                        sourceSize.height: Math.round(20 * root.u)
                    }
                    MouseArea {
                        id: unlockMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { field.forceActiveFocus(); root.submit() }
                    }
                }
            }

            // Messages: a fixed block, so nothing jumps when one appears.
            Item {
                width: parent.width
                height: Math.round(70 * root.u)

                Column {
                    width: parent.width
                    spacing: Math.round(T.spaceXs * root.u)

                    Row {
                        visible: root.errorText !== ""
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: Math.round(T.spaceSm * root.u)
                        Image {
                            anchors.verticalCenter: parent.verticalCenter
                            source: "icons/alert-triangle-danger.svg"
                            sourceSize.width: Math.round(16 * root.u)
                            sourceSize.height: Math.round(16 * root.u)
                        }
                        Text {
                            width: Math.min(implicitWidth, card.width - Math.round(26 * root.u))
                            wrapMode: Text.WordWrap
                            text: root.errorText
                            color: T.danger
                            font.family: T.fontUi
                            font.pixelSize: Math.round(T.fontBody * root.u)
                            font.weight: Font.Medium
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: text
                        }
                    }
                    Row {
                        visible: root.capsLock
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: Math.round(T.spaceSm * root.u)
                        Image {
                            anchors.verticalCenter: parent.verticalCenter
                            source: "icons/alert-triangle-warning.svg"
                            sourceSize.width: Math.round(16 * root.u)
                            sourceSize.height: Math.round(16 * root.u)
                        }
                        Text {
                            text: qsTr("Caps Lock is on")
                            color: T.warning
                            font.family: T.fontUi
                            font.pixelSize: Math.round(T.fontBody * root.u)
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: text
                        }
                    }
                    Text {
                        width: parent.width
                        visible: root.infoText !== ""
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        text: root.infoText
                        color: T.textSecondary
                        font.family: T.fontUi
                        font.pixelSize: Math.round(T.fontBody * root.u)
                    }
                    Text {
                        width: parent.width
                        visible: !root.hasAuthenticator
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("The screen locker did not provide an authenticator; unlocking is not possible from this screen.")
                        color: T.danger
                        font.family: T.fontUi
                        font.pixelSize: Math.round(T.fontBody * root.u)
                    }
                }
            }
        }
    }

    Component.onCompleted: field.forceActiveFocus()
}
