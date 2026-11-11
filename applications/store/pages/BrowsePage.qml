// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.store

// The applications of one SHADOW category.
PkListPage {
    id: page

    required property QtObject store
    property string categoryKey: ""
    readonly property var category: {
        const cats = store.catalog.categories
        for (let i = 0; i < cats.length; i++) if (cats[i].key === categoryKey) return cats[i]
        return null
    }

    signal openApp(string id)

    onCategoryKeyChanged: {
        filterField.text = ""
        order.currentIndex = 0
        view.pinned = true
        view.goToTop()
    }

    title: category ? category.title : ""
    subtitle: !category ? ""
        : store.catalog.loading ? qsTr("Loading the application catalogue…")
        : qsTr("%1 applications in the catalogue (AppStream categories: %2)").arg(category.count).arg(category.appstreamCategories.join(", "))

    model: store.categoryApps
    delegate: AppRow {
        width: page.contentWidth
        onClicked: page.openApp(appId)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd
        NxSearchField {
            id: filterField
            Layout.fillWidth: true
            Layout.maximumWidth: 360
            placeholderText: qsTr("Filter this category")
            onTextChanged: page.store.categoryApps.filter = text
        }
        Item { Layout.fillWidth: true }
        Text {
            text: qsTr("%1 shown").arg(page.count)
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
        NxSegmented {
            id: order
            model: [qsTr("A–Z"), qsTr("Newest release")]
            onActivated: (index) => { if (index === 0) page.store.categoryApps.sortByName(); else page.store.categoryApps.sortByRelease() }
        }
    }

    footer: Item {
        width: page.contentWidth
        height: empty.visible ? empty.implicitHeight : 0
        NxEmptyState {
            id: empty
            anchors.fill: parent
            visible: page.count === 0 && !page.store.catalog.loading
            icon: "search"
            title: filterField.text !== "" ? qsTr("No application in this category matches the filter")
                 : qsTr("The catalogue has no application in this category")
            message: filterField.text !== "" ? ""
                : page.store.catalog.applicationCount === 0
                    ? qsTr("No application catalogue has been downloaded yet. Refresh the software lists from the Overview page.")
                    : qsTr("None of the applications in the enabled software sources carries this category.")
        }
    }
}
