// SPDX-License-Identifier: GPL-3.0-or-later
// Round user picture; the user's initials on the brand gradient when there
// is no picture (or only SDDM's generic placeholder).
import QtQuick 2.15
import "../tokens.js" as T

Item {
    id: avatar

    property string source: ""
    property string name: ""
    property real size: 96
    readonly property bool hasPicture: picture.status === Image.Ready

    width: size
    height: size

    function initials(text) {
        var words = String(text).trim().split(/[\s._-]+/).filter(function (w) { return w.length > 0 })
        if (words.length === 0)
            return ""
        var first = words[0].charAt(0)
        var second = words.length > 1 ? words[words.length - 1].charAt(0) : ""
        return (first + second).toUpperCase()
    }

    // SDDM hands out its placeholder face for users without a picture.
    function usable(url) {
        return url !== "" && url.indexOf("/faces/.face.icon") < 0
    }

    Image {
        id: picture
        visible: false
        asynchronous: false
        cache: false
        source: avatar.usable(avatar.source) ? avatar.source : ""
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        visible: !avatar.hasPicture
        gradient: Gradient {
            GradientStop { position: 0.0; color: T.gradientStart }
            GradientStop { position: 0.6; color: T.gradientMid }
            GradientStop { position: 1.0; color: T.gradientEnd }
        }

        Text {
            anchors.centerIn: parent
            visible: text !== ""
            text: avatar.initials(avatar.name)
            color: T.textOnAccent
            font.family: T.fontUi
            font.pixelSize: Math.round(avatar.size * 0.38)
            font.weight: Font.DemiBold
        }
        Image {
            anchors.centerIn: parent
            visible: avatar.initials(avatar.name) === ""
            source: "../icons/user-on-accent.svg"
            sourceSize.width: Math.round(avatar.size * 0.5)
            sourceSize.height: Math.round(avatar.size * 0.5)
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        visible: avatar.hasPicture
        // The canvas loads the picture itself (drawing the Image item above
        // is not reliable before its first frame); the Image only tells
        // whether the file is a readable picture and how large it is.
        readonly property string url: avatar.hasPicture ? String(picture.source) : ""
        onUrlChanged: if (url !== "") { if (isImageLoaded(url)) requestPaint(); else loadImage(url) }
        onImageLoaded: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: requestPaint()
        onAvailableChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (url === "" || !isImageLoaded(url) || picture.implicitWidth <= 0 || picture.implicitHeight <= 0)
                return
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, Math.min(width, height) / 2, 0, 2 * Math.PI)
            ctx.closePath()
            ctx.clip()
            // Centre-crop to a square.
            var side = Math.min(picture.implicitWidth, picture.implicitHeight)
            ctx.drawImage(url, (picture.implicitWidth - side) / 2, (picture.implicitHeight - side) / 2, side, side,
                          0, 0, width, height)
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "transparent"
        border.width: Math.max(1, Math.round(avatar.size / 48))
        border.color: T.hoverFill
    }
}
