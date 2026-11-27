// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHADOW login screen (SDDM 0.21, Qt 6 greeter). Plain Qt Quick: the greeter
// runs before any session and without the SHADOW UI library. Colours and
// sizes come from tokens.js, generated from the SHADOW design tokens.
//
// Keyboard: the password field has the focus; Return logs in; Tab/Shift+Tab
// walk through user switch, password, reveal, log in, session, keyboard
// layout and the power buttons; lists take arrows, Return and Escape.
//
// Entrance: the screen builds up in 0.7 s behind a scan line. The form is
// there, focused and usable from the first frame; the entrance only changes
// opacity and a few pixels of offset. All decoration (scan line, ring,
// brackets, wings) is components/LoginDecor.qml behind a Loader; this file
// reads one value from it, "ent". The brackets of the shut-down question
// sit behind a Loader of their own. No decoration file is a static
// dependency of this one. reduceMotion=true in theme.conf switches every
// movement off.
import QtQuick 2.15
import SddmComponents 2.0
import "components"
import "tokens.js" as T

Rectangle {
    id: root

    // The near-black field the boot splash ends in: what the first frame
    // shows, and what stays when there is no wallpaper.
    color: T.bgBase
    gradient: Gradient {
        GradientStop { position: 0.0; color: T.fieldTop }
        GradientStop { position: 1.0; color: T.fieldBottom }
    }
    width: 1600
    height: 900

    // One design unit: the layout is drawn for 1280×800 and grows with the
    // screen, so it holds from 1024×768 to 4K without HiDPI scaling too.
    readonly property real u: Math.max(0.9, Math.min(3, Math.min(width / 1280, height / 800)))
    // SDDM loads this file once per screen; only the primary one logs in.
    readonly property bool isPrimary: typeof primaryScreen === "undefined" ? true : primaryScreen

    property int userIndex: -1            // row of userModel; -1 = type the user name
    property int sessionIndex: 0
    property bool busy: false
    property string errorText: ""
    property string infoText: ""
    property bool capsGuess: false        // from typed letters, see NxField.letterTyped
    property date now: new Date()
    property string pendingPower: ""      // "reboot" | "poweroff" awaiting confirmation

    readonly property bool manualUser: userIndex < 0 || userIndex >= users.count
    readonly property var currentUser: manualUser ? null : users.itemAt(userIndex)
    readonly property var currentSession: sessionIndex >= 0 && sessionIndex < sessions.count ? sessions.itemAt(sessionIndex) : null
    readonly property bool capsLock: (typeof keyboard !== "undefined" && keyboard.capsLock === true) || capsGuess
    readonly property var layouts: typeof keyboard !== "undefined" && keyboard.layouts ? keyboard.layouts : []
    readonly property bool anyPowerAction: sddm.canSuspend || sddm.canReboot || sddm.canPowerOff
    // Locale time format without seconds, unless theme.conf names one.
    readonly property string timeFormat: config.timeFormat ? config.timeFormat
        : Qt.locale().timeFormat(Locale.ShortFormat).replace(/[:.]s+/, "")
    readonly property bool confirmPower: String(config.confirmPower) !== "false"
    // No entrance, no shake, no unfolding lists (theme.conf; system-wide).
    readonly property bool reduceMotion: String(config.reduceMotion) === "true"

    // Progress of the entrance, 0..1, counted by the decoration. Without the
    // decoration (the file did not load) it is 1: a complete screen.
    readonly property real ent: decor.status === Loader.Ready ? decor.item.t : 1

    // Share of the window from..to (milliseconds of the entrance) that has
    // passed, slowing down towards its end. Every item that appears takes
    // its opacity from one such window.
    function ph(from, to) {
        var x = 1 - Math.max(0, Math.min(1, (ent * T.motionEntrance - from) / (to - from)))
        return 1 - x * x * x
    }
    // The buttons of the bottom bar follow each other, 40 ms apart.
    function barIn(position) {
        var from = T.motionSweep + position * T.motionStagger
        return ph(from, Math.min(T.motionEntrance, from + T.motionFast))
    }

    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    TextConstants { id: strings }

    function displayName(user) {
        return user === null ? "" : (user.realName !== "" ? user.realName : user.name)
    }

    function login() {
        if (busy)
            return
        var name = manualUser ? userField.text.trim() : currentUser.name
        if (name === "") {
            errorText = strings.promptUser
            userField.forceActiveFocus()
            return
        }
        errorText = ""
        infoText = ""
        busy = true
        answerTimeout.restart()
        sddm.login(name, passwordField.text, sessionIndex)
    }

    function selectUser(index) {
        userIndex = index
        passwordField.text = ""
        passwordField.revealed = false
        errorText = ""
        infoText = ""
        focusInput()
    }

    function focusInput() {
        if (manualUser && userField.text === "")
            userField.forceActiveFocus()
        else
            passwordField.forceActiveFocus()
    }

    function userEntries() {
        var list = []
        for (var i = 0; i < users.count; ++i) {
            var user = users.itemAt(i)
            if (!user)
                continue
            list.push({ title: displayName(user), subtitle: user.realName !== "" ? user.name : "",
                        picture: user.icon, avatar: true, avatarName: displayName(user) })
        }
        list.push({ title: qsTr("Other user…"), subtitle: strings.promptUser, picture: "", avatar: true, avatarName: "" })
        return list
    }

    function sessionEntries() {
        var list = []
        for (var i = 0; i < sessions.count; ++i) {
            var session = sessions.itemAt(i)
            if (session)
                list.push({ title: session.name, subtitle: session.comment })
        }
        return list
    }

    function layoutEntries() {
        var list = []
        for (var i = 0; i < layouts.length; ++i)
            list.push({ title: layouts[i].longName, subtitle: String(layouts[i].shortName).toUpperCase() })
        return list
    }

    function closePopups() {
        userList.hide()
        sessionList.hide()
        layoutList.hide()
    }

    function power(action) {
        if (action === "suspend") {
            sddm.suspend()
        } else if (confirmPower && pendingPower !== action) {
            closePopups()
            pendingPower = action
            cancelPower.forceActiveFocus()
        } else {
            pendingPower = ""
            focusInput()
            if (action === "reboot")
                sddm.reboot()
            else if (action === "poweroff")
                sddm.powerOff()
        }
    }

    Component.onCompleted: {
        if (users.count > 0)
            userIndex = Math.max(0, Math.min(users.count - 1, userModel.lastIndex))
        if (sessions.count > 0)
            sessionIndex = Math.max(0, Math.min(sessions.count - 1, sessionModel.lastIndex))
        if (isPrimary)
            focusInput()
    }

    Connections {
        target: sddm
        function onLoginFailed() {
            answerTimeout.stop()
            root.busy = false
            root.errorText = strings.loginFailed
            passwordField.text = ""
            passwordField.forceActiveFocus()
            if (!root.reduceMotion)
                shake.restart()
            if (decor.item)
                decor.item.fail()
        }
        function onLoginSucceeded() {
            answerTimeout.stop()
            root.errorText = ""
            root.infoText = strings.loginSucceeded
            // The form stays as it is: the greeter may be stopped at any
            // frame, so every frame has to be a complete picture.
            if (decor.item)
                decor.item.succeed()
        }
        function onInformationMessage(message) {
            root.infoText = message
        }
    }

    // The daemon answers every login with loginFailed or loginSucceeded. Should
    // it not (it crashed, the socket broke), the form must not stay locked.
    Timer {
        id: answerTimeout
        interval: 45000
        onTriggered: {
            root.busy = false
            passwordField.forceActiveFocus()
        }
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = new Date()
    }

    // The models are read through invisible delegates: one item per row,
    // addressable by index.
    Repeater {
        id: users
        model: userModel
        delegate: Item {
            visible: false
            property string name: model.name
            property string realName: model.realName ? model.realName : ""
            property string icon: model.icon ? model.icon : ""
        }
    }
    Repeater {
        id: sessions
        model: sessionModel
        delegate: Item {
            visible: false
            property string name: model.name
            property string comment: model.comment ? model.comment : ""
        }
    }

    // ---- background ---------------------------------------------------------------
    // The scan line of the entrance resolves the picture from the top: the
    // picture sits in a clip whose lower edge is the line (0–420 ms).
    // Nothing of it is shown before the entrance starts.
    Item {
        width: root.width
        height: Math.round(root.height * Math.min(1, root.ent * T.motionEntrance / T.motionSweep))
        clip: true

        Item {
            width: root.width
            height: root.height
            // A picture that is decoded only after the entrance has started
            // fades in when it arrives: behind the line if the line is still
            // on its way, as a whole if it has passed.
            opacity: wallpaper.status === Image.Ready ? 1 : 0
            Behavior on opacity {
                enabled: root.ent > 0 && !root.reduceMotion
                NumberAnimation { duration: 300 }
            }

            Image {
                id: wallpaper
                anchors.fill: parent
                source: config.background ? config.background : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                cache: false
                // Decoded at screen size, not at the size of the file.
                sourceSize.width: root.width
                sourceSize.height: root.height
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
    // Clicking beside an open list closes it.
    MouseArea {
        anchors.fill: parent
        onClicked: {
            root.closePopups()
            if (root.isPrimary && root.pendingPower === "")
                root.focusInput()
        }
    }

    // ---- decoration and the clock of the entrance ------------------------------------
    // Scan line, ring, wings and brackets are one file behind this Loader,
    // drawn under the form. Should it not load, the form below is complete
    // and works without it (ent is 1 then).
    Loader {
        id: decor
        anchors.fill: parent
        source: "components/LoginDecor.qml"
    }
    Binding { target: decor.item; property: "u"; value: root.u; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "primary"; value: root.isPrimary; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "reduce"; value: root.reduceMotion; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "now"; value: root.now; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "busy"; value: root.busy; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "focused"; value: passwordField.ringed; when: decor.status === Loader.Ready }
    Binding { target: decor.item; property: "caps"; value: root.capsLock; when: decor.status === Loader.Ready }
    // The user list or the shut-down question covers the card.
    Binding { target: decor.item; property: "overlay"; value: userList.shown || root.pendingPower !== ""; when: decor.status === Loader.Ready }
    // Where the ring and the brackets belong: around the user picture and
    // around the password row, wherever the card is (it moves when it shakes).
    Binding {
        target: decor.item; property: "ringCenter"; when: decor.status === Loader.Ready
        value: Qt.point(loginArea.x + card.x + ringSlot.x + ringSlot.width / 2,
                        loginArea.y + card.y + ringSlot.y + ringSlot.height / 2)
    }
    Binding {
        target: decor.item; property: "fieldRect"; when: decor.status === Loader.Ready
        value: Qt.rect(loginArea.x + card.x + passwordRow.x, loginArea.y + card.y + passwordRow.y,
                       passwordRow.width, passwordRow.height)
    }

    // ---- clock --------------------------------------------------------------------
    Column {
        id: clock
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(root.height * 0.085)
        spacing: Math.round(2 * root.u)

        // 180–400 ms of the entrance: the time settles 12u downwards into
        // its place; the date follows 60 ms later.
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Qt.formatTime(root.now, root.timeFormat)
            color: T.textPrimary
            font.family: T.fontUi
            font.pixelSize: Math.round(76 * root.u)
            font.weight: Font.Light
            font.letterSpacing: -1 * root.u
            opacity: root.ph(180, 400)
            transform: Translate { y: -12 * root.u * (1 - root.ph(180, 400)) }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.now.toLocaleDateString(Qt.locale(), Locale.LongFormat)
            color: T.textSecondary
            font.family: T.fontUi
            font.pixelSize: Math.round(T.fontTitle * root.u)
            opacity: root.ph(240, 460)
            transform: Translate { y: -12 * root.u * (1 - root.ph(240, 460)) }
        }
    }

    // ---- login ----------------------------------------------------------------------
    Item {
        id: loginArea
        visible: root.isPrimary
        enabled: root.pendingPower === ""
        anchors.top: clock.bottom
        anchors.bottom: bottomBar.top
        anchors.left: parent.left
        anchors.right: parent.right

        Column {
            id: card
            width: Math.round(320 * root.u)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: shakeOffset
            anchors.verticalCenter: parent.verticalCenter
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

                Avatar {
                    anchors.centerIn: parent
                    size: Math.round(104 * root.u)
                    source: root.currentUser ? root.currentUser.icon : ""
                    name: root.manualUser ? userField.text : root.displayName(root.currentUser)
                    opacity: root.ph(160, 320)
                }
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: !root.manualUser
                text: root.displayName(root.currentUser)
                elide: Text.ElideRight
                color: T.textPrimary
                font.family: T.fontUi
                font.pixelSize: Math.round(T.fontHeading * root.u)
                font.weight: Font.DemiBold
                opacity: root.ph(220, 400)
            }

            NxButton {
                id: switchUser
                u: root.u
                anchors.horizontalCenter: parent.horizontalCenter
                // With no listed user there is nothing to switch to.
                visible: users.count > 0
                implicitHeight: Math.round(30 * root.u)
                icon: "users"
                iconSize: 16 * root.u
                text: qsTr("Switch user")
                opacity: (enabled ? 1 : 0.42) * root.ph(240, 420)
                onClicked: userList.open ? userList.hide() : userList.show()
            }

            NxField {
                id: userField
                u: root.u
                width: parent.width
                visible: root.manualUser
                enabled: !root.busy
                animateFocus: root.ent >= 1 && !root.reduceMotion
                // Like the password row: there from the first frame.
                opacity: (enabled ? 1 : 0.6) * (0.55 + 0.45 * root.ph(0, 200))
                placeholder: strings.userName
                KeyNavigation.down: passwordField
                onAccepted: passwordField.forceActiveFocus()
                onLetterTyped: function (upper, shift) { root.capsGuess = (upper !== shift) }
            }

            // Field and button exist, are enabled and focused from the first
            // frame; the entrance only raises them from 55 % to full opacity
            // (0–200 ms). They are never hidden, disabled or loaded later.
            Row {
                id: passwordRow
                width: parent.width
                spacing: Math.round(T.spaceSm * root.u)
                opacity: 0.55 + 0.45 * root.ph(0, 200)

                NxField {
                    id: passwordField
                    u: root.u
                    width: parent.width - loginButton.width - parent.spacing
                    secret: true
                    enabled: !root.busy
                    animateFocus: root.ent >= 1 && !root.reduceMotion
                    error: root.errorText !== ""
                    placeholder: strings.password
                    showText: strings.showPasswordPrompt
                    hideText: strings.hidePasswordPrompt
                    KeyNavigation.up: userField.visible ? userField : null
                    onAccepted: root.login()
                    onTextChanged: if (text !== "") root.errorText = ""
                    onLetterTyped: function (upper, shift) { root.capsGuess = (upper !== shift) }
                }
                NxButton {
                    id: loginButton
                    u: root.u
                    primary: true
                    enabled: !root.busy
                    icon: "arrow-right-on-accent"
                    description: strings.login
                    onClicked: root.login()
                }
            }

            // Messages: a fixed block, so the card does not jump when one appears.
            Item {
                width: parent.width
                height: Math.round(64 * root.u)

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
                            text: strings.capslockWarning
                            color: T.warning
                            font.family: T.fontUi
                            font.pixelSize: Math.round(T.fontBody * root.u)
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: text
                        }
                    }
                    Text {
                        width: parent.width
                        visible: root.infoText !== "" || root.busy
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        text: root.infoText !== "" ? root.infoText : "…"
                        color: T.textSecondary
                        font.family: T.fontUi
                        font.pixelSize: Math.round(T.fontBody * root.u)
                    }
                }
            }
        }
    }

    // ---- bottom bar: session, keyboard layout | power ---------------------------------
    // 420–700 ms of the entrance: the buttons rise 8u into place one after
    // the other. They take keys and clicks from the first frame.
    Item {
        id: bottomBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Math.round(T.spaceXl * root.u)
        height: Math.round(T.controlHeight * root.u)
        visible: root.isPrimary
        enabled: root.pendingPower === ""

        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: Math.round(T.spaceSm * root.u)

            NxButton {
                id: sessionButton
                u: root.u
                icon: "monitor"
                trailingIcon: sessions.count > 1 ? "chevron-up" : ""
                enabled: sessions.count > 1
                opacity: root.barIn(0)
                transform: Translate { y: 8 * root.u * (1 - root.barIn(0)) }
                text: root.currentSession ? root.currentSession.name : qsTr("No session installed")
                description: strings.session
                onClicked: sessionList.open ? sessionList.hide() : sessionList.show()
            }
            NxButton {
                id: layoutButton
                u: root.u
                visible: root.layouts.length > 0
                icon: "keyboard"
                trailingIcon: root.layouts.length > 1 ? "chevron-up" : ""
                enabled: root.layouts.length > 1
                opacity: root.barIn(1)
                transform: Translate { y: 8 * root.u * (1 - root.barIn(1)) }
                text: root.layouts.length > 0 && keyboard.currentLayout >= 0 && keyboard.currentLayout < root.layouts.length
                      ? String(root.layouts[keyboard.currentLayout].shortName).toUpperCase() : ""
                description: strings.layout
                onClicked: layoutList.open ? layoutList.hide() : layoutList.show()
            }
        }

        Text {
            anchors.centerIn: parent
            visible: root.width >= 1000 * root.u
            text: sddm.hostName
            opacity: root.barIn(2)
            color: T.textMuted
            font.family: T.fontUi
            font.pixelSize: Math.round(T.fontSmall * root.u)
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Math.round(T.spaceXs * root.u)

            NxButton {
                u: root.u
                visible: sddm.canSuspend
                opacity: (enabled ? 1 : 0.42) * root.barIn(2)
                transform: Translate { y: 8 * root.u * (1 - root.barIn(2)) }
                icon: "moon"
                text: strings.suspend
                onClicked: root.power("suspend")
            }
            NxButton {
                id: rebootButton
                u: root.u
                visible: sddm.canReboot
                opacity: (enabled ? 1 : 0.42) * root.barIn(3)
                transform: Translate { y: 8 * root.u * (1 - root.barIn(3)) }
                icon: "refresh"
                text: strings.reboot
                onClicked: root.power("reboot")
            }
            NxButton {
                id: powerOffButton
                u: root.u
                visible: sddm.canPowerOff
                opacity: (enabled ? 1 : 0.42) * root.barIn(4)
                transform: Translate { y: 8 * root.u * (1 - root.barIn(4)) }
                icon: "power"
                text: strings.shutdown
                onClicked: root.power("poweroff")
            }
        }
    }

    // ---- lists ------------------------------------------------------------------------
    // "shown" is true while a list is open and while it fades out afterwards.
    NxPopupList {
        id: userList
        u: root.u
        opener: switchUser
        reduce: root.reduceMotion
        hasSubtitles: true
        width: Math.round(340 * root.u)
        // Between the clock and the bottom bar.
        readonly property real listTop: clock.y + clock.height + Math.round(T.spaceLg * root.u)
        readonly property real listRoom: bottomBar.y - Math.round(T.spaceLg * root.u) - listTop
        maximumHeight: listRoom
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(listTop + (listRoom - height) / 2)
        entries: shown ? root.userEntries() : []
        currentIndex: root.manualUser ? users.count : root.userIndex
        onChosen: function (index) { root.selectUser(index >= users.count ? -1 : index) }
    }
    NxPopupList {
        id: sessionList
        u: root.u
        opener: sessionButton
        reduce: root.reduceMotion
        title: strings.session
        hasSubtitles: true
        maximumHeight: Math.round(root.height * 0.6)
        x: bottomBar.x
        y: bottomBar.y - height - Math.round(T.spaceSm * root.u)
        entries: shown ? root.sessionEntries() : []
        currentIndex: root.sessionIndex
        onChosen: function (index) { root.sessionIndex = index; root.focusInput() }
    }
    NxPopupList {
        id: layoutList
        u: root.u
        opener: layoutButton
        reduce: root.reduceMotion
        title: strings.layout
        hasSubtitles: true
        maximumHeight: Math.round(root.height * 0.6)
        x: Math.min(root.width - width - bottomBar.x, bottomBar.x + layoutButton.x)
        y: bottomBar.y - height - Math.round(T.spaceSm * root.u)
        entries: shown ? root.layoutEntries() : []
        currentIndex: typeof keyboard !== "undefined" ? keyboard.currentLayout : -1
        onChosen: function (index) { keyboard.currentLayout = index; root.focusInput() }
    }

    // ---- confirmation for restart / shut down ---------------------------------------------
    // The screen dims in 160 ms, the card appears in 120 ms and the brackets
    // close around it; Cancel has the focus from the first frame.
    Item {
        id: confirmLayer
        anchors.fill: parent
        visible: root.pendingPower !== ""
        // Milliseconds since the question appeared.
        property real shownMs: 160
        function passed(from, to) {
            var x = 1 - Math.max(0, Math.min(1, (shownMs - from) / (to - from)))
            return 1 - x * x * x
        }
        onVisibleChanged: {
            if (visible && !root.reduceMotion) {
                appear.restart()
            } else {
                appear.stop()
                shownMs = 160
            }
        }
        NumberAnimation { id: appear; target: confirmLayer; property: "shownMs"; from: 0; to: 160; duration: 160 }

        Rectangle {
            anchors.fill: parent
            color: T.bgBase
            opacity: 0.6 * Math.min(1, confirmLayer.shownMs / 160)
        }
        MouseArea { anchors.fill: parent; hoverEnabled: true }

        // Decoration, so behind a Loader like the rest of it: should the
        // file not load, the question works without its brackets.
        Loader {
            id: confirmBrackets
            anchors.fill: confirm
            source: "components/Brackets.qml"
        }
        Binding { target: confirmBrackets.item; property: "u"; value: root.u; when: confirmBrackets.status === Loader.Ready }
        Binding { target: confirmBrackets.item; property: "color"; value: T.accent; when: confirmBrackets.status === Loader.Ready }
        Binding {
            target: confirmBrackets.item; property: "spread"; when: confirmBrackets.status === Loader.Ready
            value: (8 + 12 * (1 - confirmLayer.passed(20, 160))) * root.u
        }
        Binding {
            target: confirmBrackets.item; property: "opacity"; when: confirmBrackets.status === Loader.Ready
            value: confirmLayer.passed(20, 160)
        }

        FocusScope {
            id: confirm
            anchors.centerIn: parent
            width: Math.round(380 * root.u)
            height: confirmColumn.implicitHeight + 2 * Math.round(T.spaceXl * root.u)
            opacity: confirmLayer.passed(0, 120)

            function dismiss() {
                var back = root.pendingPower === "reboot" ? rebootButton : powerOffButton
                root.pendingPower = ""
                back.forceActiveFocus()
            }
            Keys.onEscapePressed: dismiss()

            Rectangle {
                anchors.fill: parent
                radius: Math.round(T.radiusXl * root.u)
                color: T.glassPopup
                border.width: 1
                border.color: T.border
            }
            Column {
                id: confirmColumn
                anchors.centerIn: parent
                width: parent.width - 2 * Math.round(T.spaceXl * root.u)
                spacing: Math.round(T.spaceLg * root.u)

                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: root.pendingPower === "reboot" ? "icons/refresh.svg" : "icons/power.svg"
                    sourceSize.width: Math.round(32 * root.u)
                    sourceSize.height: Math.round(32 * root.u)
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: root.pendingPower === "reboot" ? qsTr("Restart the computer?") : qsTr("Shut down the computer?")
                    color: T.textPrimary
                    font.family: T.fontUi
                    font.pixelSize: Math.round(T.fontTitle * root.u)
                    font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Other users who are still logged in lose unsaved work.")
                    color: T.textSecondary
                    font.family: T.fontUi
                    font.pixelSize: Math.round(T.fontBody * root.u)
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Math.round(T.spaceMd * root.u)
                    NxButton {
                        id: cancelPower
                        u: root.u
                        flat: false
                        focus: true
                        text: qsTr("Cancel")
                        KeyNavigation.right: confirmPowerButton
                        onClicked: confirm.dismiss()
                    }
                    NxButton {
                        id: confirmPowerButton
                        u: root.u
                        danger: true
                        text: root.pendingPower === "reboot" ? strings.reboot : strings.shutdown
                        KeyNavigation.left: cancelPower
                        onClicked: root.power(root.pendingPower)
                    }
                }
            }
        }
    }
}
