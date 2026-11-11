// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Appearance")
    subtitle: qsTr("Colours, wallpaper, dock and panel of the NEXUS desktop. Changes apply immediately to every NEXUS application.")

    // Which wallpaper slot the chooser edits: the shell shows one image in
    // dark and another in light appearance.
    property bool editLight: !Appearance.dark
    readonly property string currentWallpaper: editLight ? backend.wallpaperLight : backend.wallpaperDark
    readonly property var fillModes: [
        { value: "crop", text: qsTr("Fill the screen") },
        { value: "fit", text: qsTr("Fit inside the screen") },
        { value: "stretch", text: qsTr("Stretch") },
        { value: "center", text: qsTr("Centre") },
        { value: "tile", text: qsTr("Tile") },
    ]

    // The UI library stores the text size; whether its tokens follow is
    // observed, not assumed (see README-system.md).
    property real scaleAtLoad: 1
    property int bodySizeAtLoad: 0
    readonly property bool fontScaleIgnored: Math.abs(Appearance.fontScale - scaleAtLoad) > 0.01
                                             && Theme.fontSizeBody === bodySizeAtLoad
    Component.onCompleted: { scaleAtLoad = Appearance.fontScale; bodySizeAtLoad = Theme.fontSizeBody }

    AppearanceBackend {
        id: backend
        onFailed: (message) => page.ApplicationWindow.window.toaster.show(message, "danger")
    }

    // ---- colours -------------------------------------------------------------
    NxSection {
        title: qsTr("Colours")

        NxRow {
            icon: Appearance.dark ? "moon" : "sun"
            title: qsTr("Appearance")
            subtitle: qsTr("Dark or light surfaces in all NEXUS applications and the shell.")
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("Dark"), qsTr("Light")]
                currentIndex: Appearance.dark ? 0 : 1
                onActivated: (index) => {
                    Appearance.dark = index === 0
                    currentIndex = Qt.binding(() => Appearance.dark ? 0 : 1)
                }
            }
        }
        NxRow {
            icon: "palette"
            title: qsTr("Accent colour")
            subtitle: Theme.accentLabels[Appearance.accent] || Appearance.accent
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceSm
                Repeater {
                    model: Theme.accentNames
                    Rectangle {
                        id: swatch
                        required property string modelData
                        readonly property bool selected: Appearance.accent === modelData
                        width: 28
                        height: 28
                        radius: 14
                        color: "transparent"
                        border.width: selected || swatch.activeFocus ? 2 : 0
                        border.color: Theme.accentByName(modelData)
                        activeFocusOnTab: true
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: Theme.accentLabels[modelData] || modelData
                        Accessible.checked: selected
                        Keys.onReturnPressed: Appearance.accent = modelData
                        Keys.onSpacePressed: Appearance.accent = modelData

                        Rectangle {
                            anchors.centerIn: parent
                            width: 20
                            height: 20
                            radius: 10
                            color: Theme.accentByName(swatch.modelData)
                            NxIcon {
                                anchors.centerIn: parent
                                visible: swatch.selected
                                name: "check"
                                size: 12
                                color: Theme.textOnAccent
                            }
                        }
                        HoverHandler { id: swatchHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: Appearance.accent = swatch.modelData }
                        ToolTip.visible: swatchHover.hovered
                        ToolTip.text: Theme.accentLabels[modelData] || modelData
                        ToolTip.delay: 500
                    }
                }
            }
        }
        NxRow {
            icon: "layers"
            title: qsTr("Transparency")
            subtitle: qsTr("Translucent, blurred panel, dock and pop-ups. Needs the compositor's blur effect.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: Appearance.transparency
                Accessible.name: qsTr("Transparency")
                onToggled: Appearance.transparency = checked
            }
        }
        NxRow {
            icon: "activity"
            title: qsTr("Reduce motion")
            subtitle: qsTr("Switches off animations in NEXUS applications and the shell.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: Appearance.reduceMotion
                Accessible.name: qsTr("Reduce motion")
                onToggled: Appearance.reduceMotion = checked
            }
        }
        NxRow {
            icon: "type"
            title: qsTr("Text size")
            subtitle: page.fontScaleIgnored
                ? qsTr("Saved, but this version of the NEXUS UI library does not scale its text yet.")
                : qsTr("Scales the text of NEXUS applications.")
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: Math.round(fontSlider.value * 100) + " %"
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: fontSlider
                anchors.verticalCenter: parent.verticalCenter
                width: 220
                from: 0.8
                to: 1.6
                stepSize: 0.05
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Text size")
                // Applied on release: re-laying out the page under the pointer
                // while dragging would move the slider away from it.
                onPressedChanged: if (!pressed) Appearance.fontScale = Math.round(value * 100) / 100
                Keys.onReleased: Appearance.fontScale = Math.round(value * 100) / 100
                Binding {
                    target: fontSlider
                    property: "value"
                    value: Appearance.fontScale
                    when: !fontSlider.pressed
                    restoreMode: Binding.RestoreNone
                }
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Reset")
                enabled: Math.abs(Appearance.fontScale - 1) > 0.001
                onClicked: Appearance.fontScale = 1
            }
        }
    }

    // ---- wallpaper --------------------------------------------------------------
    NxSection {
        title: qsTr("Wallpaper")
        description: qsTr("The shell shows one image in dark and one in light appearance.")
        headerActions: NxSegmented {
            model: [qsTr("Dark appearance"), qsTr("Light appearance")]
            currentIndex: page.editLight ? 1 : 0
            onActivated: (index) => page.editLight = index === 1
        }

        Item {
            Layout.fillWidth: true
            implicitHeight: wallpaperBody.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: wallpaperBody
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceLg

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceLg

                    // Preview of the desktop: the real image in the real fill
                    // mode, scaled down by the same factor as the screen.
                    Rectangle {
                        id: preview
                        readonly property real factor: width / Math.max(1, Screen.width)
                        readonly property bool readable: backend.isReadableImage(page.currentWallpaper)
                        readonly property size natural: readable ? backend.imageSize(page.currentWallpaper) : Qt.size(0, 0)
                        readonly property bool unscaled: backend.wallpaperFill === "center" || backend.wallpaperFill === "tile"
                        Layout.preferredWidth: 320
                        Layout.preferredHeight: Math.round(320 * Screen.height / Math.max(1, Screen.width))
                        radius: Theme.radiusSm
                        color: Theme.bgBase
                        border.width: Theme.borderWidth
                        border.color: Theme.border
                        clip: true

                        Image {
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: preview.readable
                            source: preview.readable ? backend.fileUrl(page.currentWallpaper) : ""
                            asynchronous: true
                            cache: false
                            horizontalAlignment: Image.AlignHCenter
                            verticalAlignment: Image.AlignVCenter
                            sourceSize: preview.unscaled
                                ? Qt.size(Math.max(1, Math.round(preview.natural.width * preview.factor)),
                                          Math.max(1, Math.round(preview.natural.height * preview.factor)))
                                : Qt.size(640, 0)
                            fillMode: backend.wallpaperFill === "fit" ? Image.PreserveAspectFit
                                    : backend.wallpaperFill === "stretch" ? Image.Stretch
                                    : backend.wallpaperFill === "center" ? Image.Pad
                                    : backend.wallpaperFill === "tile" ? Image.Tile : Image.PreserveAspectCrop
                        }
                        Label {
                            anchors.centerIn: parent
                            width: parent.width - Theme.spaceLg * 2
                            visible: !preview.readable
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                            text: page.currentWallpaper === ""
                                ? qsTr("No wallpaper is set; the shell shows its gradient.")
                                : qsTr("%1 cannot be read; the shell shows its gradient.").arg(page.currentWallpaper)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.spaceSm

                        Label {
                            Layout.fillWidth: true
                            text: page.editLight ? qsTr("Shown in light appearance") : qsTr("Shown in dark appearance")
                            font.weight: Theme.weightSemibold
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: page.currentWallpaper
                            color: Theme.textMuted
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WrapAnywhere
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: preview.readable
                            text: qsTr("%1 × %2 pixels").arg(preview.natural.width).arg(preview.natural.height)
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        RowLayout {
                            Layout.topMargin: Theme.spaceSm
                            spacing: Theme.spaceSm
                            Label { text: qsTr("Placement"); color: Theme.textSecondary }
                            ComboBox {
                                Layout.preferredWidth: 200
                                model: page.fillModes
                                textRole: "text"
                                valueRole: "value"
                                Accessible.name: qsTr("Wallpaper placement")
                                currentIndex: {
                                    for (let i = 0; i < page.fillModes.length; i++)
                                        if (page.fillModes[i].value === backend.wallpaperFill) return i
                                    return 0
                                }
                                onActivated: backend.setWallpaperFill(currentValue)
                            }
                        }
                        RowLayout {
                            spacing: Theme.spaceSm
                            NxButton {
                                text: qsTr("Choose a file…")
                                iconName: "folder-open"
                                onClicked: fileDialog.open()
                            }
                            NxButton {
                                text: page.editLight ? qsTr("Use for dark too") : qsTr("Use for light too")
                                enabled: preview.readable && backend.wallpaperDark !== backend.wallpaperLight
                                onClicked: backend.setWallpaper(page.currentWallpaper, !page.editLight)
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: backend.wallpapers.length === 0
                    text: qsTr("No wallpapers are installed in %1.").arg(backend.wallpaperDirectory)
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                }

                GridLayout {
                    id: grid
                    Layout.fillWidth: true
                    visible: backend.wallpapers.length > 0
                    columns: 4
                    columnSpacing: Theme.spaceMd
                    rowSpacing: Theme.spaceMd

                    Repeater {
                        model: backend.wallpapers
                        Item {
                            id: tile
                            required property var modelData
                            readonly property bool selected: modelData.path === page.currentWallpaper
                            Layout.fillWidth: true
                            Layout.preferredWidth: 100
                            implicitHeight: thumbFrame.height + caption.implicitHeight + Theme.spaceXs
                            activeFocusOnTab: true
                            Accessible.role: Accessible.RadioButton
                            Accessible.name: qsTr("Wallpaper %1").arg(modelData.name)
                            Accessible.checked: selected
                            Keys.onReturnPressed: backend.setWallpaper(modelData.path, page.editLight)
                            Keys.onSpacePressed: backend.setWallpaper(modelData.path, page.editLight)

                            Rectangle {
                                id: thumbFrame
                                width: parent.width
                                height: Math.round(width * 10 / 16)
                                radius: Theme.radiusSm
                                color: Theme.bgBase
                                border.width: tile.selected || tile.activeFocus ? 2 : Theme.borderWidth
                                border.color: tile.selected ? Theme.accent
                                            : tile.activeFocus || tileHover.hovered ? Theme.borderStrong : Theme.border
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    source: tile.modelData.url
                                    sourceSize: Qt.size(400, 0)
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                }
                                Rectangle {
                                    visible: tile.selected
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceXs + 2
                                    width: 20
                                    height: 20
                                    radius: 10
                                    color: Theme.accent
                                    NxIcon { anchors.centerIn: parent; name: "check"; size: 12; color: Theme.textOnAccent }
                                }
                            }
                            Label {
                                id: caption
                                anchors.top: thumbFrame.bottom
                                anchors.topMargin: Theme.spaceXs
                                width: parent.width
                                text: tile.modelData.bundled ? tile.modelData.name : qsTr("%1 (your file)").arg(tile.modelData.name)
                                color: tile.selected ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                            HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: backend.setWallpaper(tile.modelData.path, page.editLight) }
                        }
                    }
                }
            }
        }
    }

    // ---- dock ----------------------------------------------------------------------
    NxSection {
        title: qsTr("Dock")
        NxRow {
            icon: "dock"
            title: qsTr("Hide the dock automatically")
            subtitle: qsTr("The dock slides away and returns when the pointer reaches the bottom edge.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: backend.dockAutoHide
                Accessible.name: qsTr("Hide the dock automatically")
                onToggled: backend.setDockAutoHide(checked)
            }
        }
        NxRow {
            icon: "grid"
            title: qsTr("Icon size")
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("%1 px").arg(Math.round(dockSlider.value))
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: dockSlider
                anchors.verticalCenter: parent.verticalCenter
                width: 220
                from: 32
                to: 72
                stepSize: 4
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Dock icon size")
                onMoved: backend.setDockIconSize(Math.round(value))
                Binding {
                    target: dockSlider
                    property: "value"
                    value: backend.dockIconSize
                    when: !dockSlider.pressed
                    restoreMode: Binding.RestoreNone
                }
            }
        }
    }

    // ---- panel ----------------------------------------------------------------------
    NxSection {
        title: qsTr("Panel clock")
        description: qsTr("Stored in %1, which the shell follows.").arg(backend.settingsFile)
        NxRow {
            icon: "clock"
            title: qsTr("24-hour clock")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: backend.clock24h
                Accessible.name: qsTr("24-hour clock")
                onToggled: backend.setClock24h(checked)
            }
        }
        NxRow {
            icon: "timer"
            title: qsTr("Show seconds")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: backend.clockSeconds
                Accessible.name: qsTr("Show seconds")
                onToggled: backend.setClockSeconds(checked)
            }
        }
        NxRow {
            icon: "calendar"
            title: qsTr("Show the date")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: backend.clockDate
                Accessible.name: qsTr("Show the date")
                onToggled: backend.setClockDate(checked)
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a wallpaper")
        fileMode: FileDialog.OpenFile
        nameFilters: backend.imageNameFilters
        onAccepted: backend.setWallpaper(selectedFile.toString(), page.editLight)
    }
}
