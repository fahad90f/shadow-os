// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import org.shadow.assistant
import "markdown.js" as Md

// Shadow's answers, set like a book. markdown.js splits the Markdown into a
// flat list of blocks (paragraphs, headings, list items at their nesting
// level, quotes, code, rules, tables); each block is drawn by its own
// delegate with generous leading. The list is diffed into a ListModel, so a
// streamed delta re-lays out only the block that grew, and parsing restarts
// from the last finished paragraph (Md.parseGrowing). While a reply streams,
// re-rendering is coalesced to at most 20 times a second.
//
// `highlight` is the sentence she is speaking right now (as the voice
// controller sent it); it is located by its words and marked in place.
Column {
    id: prose
    required property StageModel stage
    property string markdown: ""
    property bool streaming: false
    property string highlight: ""

    // parse state
    property var cache: ({})
    property var refs: ({})
    property string refsKey: "{}"
    property var blocks: []
    property var encs: []
    property int hlBlock: -1
    property int hlStart: 0
    property int hlEnd: 0
    // where the last spoken sentence was found: the next one is looked for
    // after it (the same words can occur earlier in the text)
    property string hlFound: ""
    property int nextBlock: 0
    property int nextPos: 0

    readonly property int indentStep: 26
    readonly property int quoteStep: 16

    // inline styling, from the tokens (Qt rich text CSS)
    readonly property var inlineStyle: ({
        codeCss: "font-family:'" + Pe.mono + "'; font-size:" + (Pe.code - 1) + "px; color:" + Pe.hex(Pe.codeSpan) + ";",
        // links: her colour and an underline (never told apart by colour alone)
        linkCss: "color:" + Pe.hex(Pe.her) + "; text-decoration:underline;",
        hlCss: "background-color:" + Pe.css(Pe.spokenMark, Pe.spokenMarkAlpha) + ";",
        hl: null
    })
    function styleWith(start: int, end: int): var {
        const s = inlineStyle
        return { codeCss: s.codeCss, linkCss: s.linkCss, hlCss: s.hlCss, hl: end > start ? [start, end] : null }
    }

    ListModel { id: blockModel }

    Timer {
        id: throttle
        interval: 50
        onTriggered: prose.sync()
    }
    onMarkdownChanged: {
        if (streaming) {
            if (!throttle.running)
                throttle.start()
        } else {
            sync()
        }
    }
    onStreamingChanged: {
        if (!streaming) {
            throttle.stop()
            sync()
        }
    }
    onHighlightChanged: locateHighlight()
    Component.onCompleted: sync()

    function sync() {
        const p = Md.parseGrowing(markdown, cache)
        const b = p.blocks
        const rk = JSON.stringify(p.refs)
        if (rk !== refsKey) {
            refs = p.refs
            refsKey = rk
        }
        const next = []
        for (let i = 0; i < b.length; ++i) {
            const pq = i > 0 ? b[i - 1].quote : 0
            let enc = b[i]._enc
            if (enc === undefined || b[i]._pq !== pq) {
                b[i].pq = pq
                enc = JSON.stringify(b[i])
                b[i]._enc = enc
                b[i]._pq = pq
            }
            next.push(enc)
            if (i < encs.length) {
                if (encs[i] !== enc)
                    blockModel.set(i, { enc: enc })
            } else {
                blockModel.append({ enc: enc })
            }
        }
        while (blockModel.count > b.length)
            blockModel.remove(blockModel.count - 1)
        encs = next
        blocks = b
        locateHighlight()
    }

    function locateHighlight() {
        if (highlight === "") {
            hlBlock = -1
            return
        }
        // the same sentence again (the text grew): where it was found;
        // a new sentence: after the one before it
        const again = highlight === hlFound && hlBlock >= 0
        const r = again ? Md.findSentence(blocks, refs, highlight, hlBlock, hlStart)
                        : Md.findSentence(blocks, refs, highlight, nextBlock, nextPos)
        if (r === null) {
            hlBlock = -1
            return
        }
        hlFound = highlight
        hlStart = r.a
        hlEnd = r.b
        hlBlock = r.block
        nextBlock = r.block
        nextPos = r.b
    }
    // The top of the block that holds the spoken sentence (0: none).
    function highlightY(): real {
        const it = blockRepeater.itemAt(hlBlock)
        return it ? it.y : 0
    }

    Repeater {
        id: blockRepeater
        model: blockModel
        delegate: Item {
            id: blockItem
            required property string enc
            required property int index
            // everything about the block comes from one role (its encoding),
            // so a block that changes kind while streaming is never seen half-updated
            readonly property var blk: JSON.parse(enc)
            readonly property string kind: blk.k || "p"
            readonly property real gap: blk.gap || 0
            readonly property int quote: blk.quote || 0
            readonly property real indentX: quote * prose.quoteStep + (blk.ind || 0) * prose.indentStep
            // the spoken sentence, when it is in this block: "start:end"
            readonly property string hlKey: index === prose.hlBlock ? prose.hlStart + ":" + prose.hlEnd : ""
            width: prose.width
            implicitHeight: gap + loader.implicitHeight

            // block-quote bars (joined to the bars of the block above)
            Repeater {
                model: blockItem.quote
                delegate: Rectangle {
                    required property int index
                    x: index * prose.quoteStep
                    y: index < (blockItem.blk.pq || 0) ? 0 : blockItem.gap
                    width: 2
                    height: blockItem.height - y
                    radius: 1
                    color: Pe.alpha(Pe.violet, 0.55)
                }
            }
            // list marker
            Text {
                textFormat: Text.PlainText
                visible: blockItem.kind === "li"
                x: blockItem.indentX - prose.indentStep
                y: blockItem.gap + (blockItem.blk.ordered ? 4 : 1)
                width: prose.indentStep - 8
                horizontalAlignment: Text.AlignRight
                text: blockItem.blk.marker || ""
                font.family: blockItem.blk.ordered ? Pe.mono : Pe.sans
                font.pixelSize: blockItem.blk.ordered ? Pe.small : (blockItem.blk.task ? 16 : 15)
                color: Pe.her
                Accessible.ignored: true
            }
            Loader {
                id: loader
                x: blockItem.indentX
                y: blockItem.gap
                width: Math.max(0, prose.width - blockItem.indentX)
                sourceComponent: blockItem.kind === "code" ? codeBlock
                               : blockItem.kind === "hr" ? ruleBlock
                               : blockItem.kind === "table" ? tableBlock
                               : textBlock
            }

            Component {
                id: textBlock
                RichBlock {
                    stage: prose.stage
                    readonly property bool heading: blockItem.kind === "h"
                    readonly property int level: blockItem.blk.level || 2
                    width: loader.width
                    color: blockItem.quote > 0 ? Pe.inkSoft : Pe.ink
                    font.pixelSize: !heading ? Pe.body : level <= 1 ? 21 : level === 2 ? 18 : level === 3 ? 16 : Pe.body
                    font.weight: heading ? Font.DemiBold : Font.Normal
                    text: {
                        if (prose.refsKey === "")  // re-render when link definitions change (a bare read could be compiled away)
                            return ""
                        const hl = blockItem.hlKey === "" ? prose.styleWith(0, 0) : prose.styleWith(prose.hlStart, prose.hlEnd)
                        return Md.paragraphHtml(blockItem.blk.s || "", prose.refs, hl, heading ? 130 : 160)
                    }
                }
            }
            Component {
                id: ruleBlock
                Item {
                    implicitHeight: 12
                    width: loader.width
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 1
                        color: Pe.line
                    }
                }
            }
            Component {
                id: codeBlock
                CodeSlab {
                    stage: prose.stage
                    width: loader.width
                    code: blockItem.blk.code || ""
                    lang: blockItem.blk.lang || ""
                    growing: prose.streaming && blockItem.blk.open === true
                }
            }
            Component {
                id: tableBlock
                Flickable {
                    id: tableFlick
                    width: loader.width
                    implicitHeight: table.implicitHeight + (contentWidth > width ? 10 : 0)
                    height: implicitHeight
                    contentWidth: Math.max(width, table.contentWidth)
                    contentHeight: table.implicitHeight
                    flickableDirection: Flickable.HorizontalFlick
                    interactive: contentWidth > width
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.horizontal: PeScrollBar {}
                    RichBlock {
                        id: table
                        stage: prose.stage
                        width: tableFlick.width
                        font.pixelSize: Pe.ui + 1
                        text: {
                            if (prose.refsKey === "")  // re-render when link definitions change (a bare read could be compiled away)
                                return ""
                            // The header row is set apart by weight and ink, not a fill:
                            // Qt's rich text paints a cell's background inside its
                            // padding only (an inset band, not a shaded cell).
                            return Md.tableHtml(blockItem.blk, prose.refs, prose.styleWith(0, 0), {
                                pad: 7, line: Pe.hex(Pe.lineStrong),
                                headCss: "color:" + Pe.hex(Pe.ink) + "; font-weight:700;"
                            })
                        }
                    }
                }
            }
        }
    }
}
