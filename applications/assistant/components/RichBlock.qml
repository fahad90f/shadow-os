// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// One block of her answer as rich text: selectable, links open in the
// browser (web and e-mail only, StageModel.openLink). The HTML comes from
// markdown.js, which never writes anything that loads (no images, no URLs in
// styles), so displaying it fetches nothing.
TextEdit {
    id: block
    required property StageModel stage
    readOnly: true
    selectByMouse: true
    persistentSelection: false
    wrapMode: TextEdit.Wrap
    textFormat: TextEdit.RichText
    color: Pe.ink
    selectionColor: Pe.alpha(Pe.violet, 0.38)
    selectedTextColor: Pe.ink
    font.family: Pe.sans
    font.pixelSize: Pe.body
    onLinkActivated: (link) => block.stage.openLink(link)
    HoverHandler {
        cursorShape: block.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor
    }
}
