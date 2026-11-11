// SPDX-License-Identifier: GPL-3.0-or-later
//
// NEXUS login screen (SDDM 0.21, Qt 6 greeter). Plain Qt Quick: the greeter
// runs before any session and without the NEXUS UI library. Colours and
// sizes come from tokens.js, generated from the NEXUS design tokens.
//
// Keyboard: the password field has the focus; Return logs in; Tab/Shift+Tab
// walk through user switch, password, reveal, log in, session, keyboard
// layout and the power buttons; lists take arrows, Return and Escape.
import QtQuick 2.15
import SddmComponents 2.0
import "components"
import "tokens.js" as T

Rectangle {
    id: root

    // Solid fallback when there is no wallpaper.
    color: T.bgBase
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
            shake.restart()
        }
        function onLoginSucceeded() {
            answerTimeout.stop()
            root.errorText = ""
            root.infoText = strings.loginSucceeded
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
    // Clicking beside an open list closes it.
    MouseArea {
        anchors.fill: parent
        onClicked: {
            root.closePopups()
            if (root.isPrimary && root.pendingPower === "")
                root.focusInput()
        }
    }

    // ---- clock --------------------------------------------------------------------
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
            font.letterSpacing: -1 * root.u
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.now.toLocaleDateString(Qt.locale(), Locale.LongFormat)
            color: T.textSecondary
            font.family: T.fontUi
            font.pixelSize: Math.round(T.fontTitle * root.u)
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

            Avatar {
                anchors.horizontalCenter: parent.horizontalCenter
                size: Math.round(104 * root.u)
                source: root.currentUser ? root.currentUser.icon : ""
                name: root.manualUser ? userField.text : root.displayName(root.currentUser)
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
                onClicked: userList.open ? userList.hide() : userList.show()
            }

            NxField {
                id: userField
                u: root.u
                width: parent.width
                visible: root.manualUser
                enabled: !root.busy
                placeholder: strings.userName
                KeyNavigation.down: passwordField
                onAccepted: passwordField.forceActiveFocus()
                onLetterTyped: function (upper, shift) { root.capsGuess = (upper !== shift) }
            }

            Row {
                width: parent.width
                spacing: Math.round(T.spaceSm * root.u)

                NxField {
                    id: passwordField
                    u: root.u
                    width: parent.width - loginButton.width - parent.spacing
                    secret: true
                    enabled: !root.busy
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
                opacity: 1
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
                opacity: 1
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
                icon: "moon"
                text: strings.suspend
                onClicked: root.power("suspend")
            }
            NxButton {
                id: rebootButton
                u: root.u
                visible: sddm.canReboot
                icon: "refresh"
                text: strings.reboot
                onClicked: root.power("reboot")
            }
            NxButton {
                id: powerOffButton
                u: root.u
                visible: sddm.canPowerOff
                icon: "power"
                text: strings.shutdown
                onClicked: root.power("poweroff")
            }
        }
    }

    // ---- lists ------------------------------------------------------------------------
    NxPopupList {
        id: userList
        u: root.u
        opener: switchUser
        hasSubtitles: true
        width: Math.round(340 * root.u)
        // Between the clock and the bottom bar.
        readonly property real listTop: clock.y + clock.height + Math.round(T.spaceLg * root.u)
        readonly property real listRoom: bottomBar.y - Math.round(T.spaceLg * root.u) - listTop
        maximumHeight: listRoom
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round(listTop + (listRoom - height) / 2)
        entries: open ? root.userEntries() : []
        currentIndex: root.manualUser ? users.count : root.userIndex
        onChosen: function (index) { root.selectUser(index >= users.count ? -1 : index) }
    }
    NxPopupList {
        id: sessionList
        u: root.u
        opener: sessionButton
        title: strings.session
        hasSubtitles: true
        maximumHeight: Math.round(root.height * 0.6)
        x: bottomBar.x
        y: bottomBar.y - height - Math.round(T.spaceSm * root.u)
        entries: open ? root.sessionEntries() : []
        currentIndex: root.sessionIndex
        onChosen: function (index) { root.sessionIndex = index; root.focusInput() }
    }
    NxPopupList {
        id: layoutList
        u: root.u
        opener: layoutButton
        title: strings.layout
        hasSubtitles: true
        maximumHeight: Math.round(root.height * 0.6)
        x: Math.min(root.width - width - bottomBar.x, bottomBar.x + layoutButton.x)
        y: bottomBar.y - height - Math.round(T.spaceSm * root.u)
        entries: open ? root.layoutEntries() : []
        currentIndex: typeof keyboard !== "undefined" ? keyboard.currentLayout : -1
        onChosen: function (index) { keyboard.currentLayout = index; root.focusInput() }
    }

    // ---- confirmation for restart / shut down ---------------------------------------------
    Rectangle {
        anchors.fill: parent
        visible: root.pendingPower !== ""
        color: T.scrimBottom
        MouseArea { anchors.fill: parent; hoverEnabled: true }

        FocusScope {
            id: confirm
            anchors.centerIn: parent
            width: Math.round(380 * root.u)
            height: confirmColumn.implicitHeight + 2 * Math.round(T.spaceXl * root.u)

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
