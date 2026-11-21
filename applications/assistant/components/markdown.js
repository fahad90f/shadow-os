// SPDX-License-Identifier: GPL-3.0-or-later
.pragma library

// Markdown for Shadow's answers (CommonMark + the GitHub extensions the model
// uses), turned into a FLAT list of blocks that the UI draws one by one, and
// into Qt rich text for the prose inside them.
//
// Why our own: the UI needs control that Text.MarkdownText does not give —
// book-like leading, the token colours, code slabs with Copy, a highlight on
// the sentence being spoken — and, above all, privacy: nothing in a reply may
// make the app fetch anything. Here everything is escaped by construction:
// raw HTML stays text, a Markdown image becomes a link (its alt text), and the
// only tags in the output are the few this file writes (p, br, b, i, s, span,
// a, table/tr/td/th). No <img>, no CSS url(), nothing to load.
//
// Streaming: parse() is cheap (one pass over the lines) and the UI diffs the
// block list, so a delta only re-lays out the block that grew.
//
// Blocks (all carry ind = content indent level, quote = block-quote depth):
//   {k:"p",  s}                      paragraph (s = inline Markdown, \n = soft break)
//   {k:"li", s, marker, ordered}     list item: its first paragraph; ind = level + 1
//   {k:"h",  s, level}               heading 1..6
//   {k:"code", code, lang, open}     fenced/indented code (open: fence not closed yet)
//   {k:"hr"}                         thematic break
//   {k:"table", head, align, rows}   GitHub table (cells are inline Markdown)
//   gap: suggested space above the block (px)

// =============================================================================
// block structure
// =============================================================================

var RE_HR = /^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$/;
var RE_ATX = /^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$/;
var RE_SETEXT1 = /^ {0,3}=+[ \t]*$/;
var RE_SETEXT2 = /^ {0,3}-+[ \t]*$/;
var RE_FENCE = /^( {0,3})(`{3,}|~{3,})[ \t]*(.*?)[ \t]*$/;
var RE_ITEM = /^( *)([-*+]|\d{1,9}[.)])(?:([ \t]+)(.*))?$/;
var RE_REFDEF = /^ {0,3}\[([^\]]+)\]:[ \t]*<?([^\s>]+)>?(?:[ \t]+(?:"[^"]*"|'[^']*'|\([^)]*\)))?[ \t]*$/;
var RE_TABLE_SEP = /^ {0,3}\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*$/;
var BULLETS = ["•", "◦", "▪"];     // • ◦ ▪ by nesting level

function expandTabs(line) {
    if (line.indexOf("\t") < 0)
        return line;
    var out = "", col = 0;
    for (var i = 0; i < line.length; ++i) {
        var c = line.charAt(i);
        if (c === "\t") {
            var n = 4 - (col % 4);
            out += "    ".slice(0, n);
            col += n;
        } else {
            out += c;
            ++col;
        }
    }
    return out;
}

function indentOf(s) {
    var n = 0;
    while (n < s.length && s.charAt(n) === " ")
        ++n;
    return n;
}

// Leading block-quote markers: {q: depth, rest: text after them}.
function stripQuotes(line, max) {
    var q = 0, rest = line;
    while (max === undefined || q < max) {
        var m = /^ {0,3}> ?/.exec(rest);
        if (!m)
            break;
        rest = rest.slice(m[0].length);
        ++q;
    }
    return { q: q, rest: rest };
}

function normLabel(s) {
    return s.trim().replace(/\s+/g, " ").toLowerCase();
}

// Cells of a table row; "\|" stays escaped (the inline parser makes it a "|").
function splitCells(line) {
    var s = line.trim();
    if (s.charAt(0) === "|")
        s = s.slice(1);
    if (s.length && s.charAt(s.length - 1) === "|" && s.charAt(s.length - 2) !== "\\")
        s = s.slice(0, -1);
    var cells = [], cur = "";
    for (var i = 0; i < s.length; ++i) {
        var c = s.charAt(i);
        if (c === "\\" && i + 1 < s.length) {
            cur += c + s.charAt(i + 1);
            ++i;
        } else if (c === "|") {
            cells.push(cur.trim());
            cur = "";
        } else {
            cur += c;
        }
    }
    cells.push(cur.trim());
    return cells;
}

function hasPipe(line) {
    return /(^|[^\\])\|/.test(line);
}

function parse(src) {
    var lines = String(src || "").replace(/\r\n?/g, "\n").split("\n");
    var blocks = [];
    var refs = {};
    var quote = 0;          // block-quote depth of the open structures
    var lists = [];         // open lists: {marker, content, ordered, num, level}
    var para = null;        // open paragraph-like block (p / li / h-to-be)
    var fence = null;       // open fenced code block
    var icode = null;       // open indented code block
    var table = null;       // open table
    var blank = false;      // the previous line was blank
    var afterItemBlank = false;

    function emit(b) {
        b.quote = b.quote === undefined ? quote : b.quote;
        blocks.push(b);
        return b;
    }
    function closePara() {
        if (!para)
            return;
        para.s = para.lines.join("\n");
        delete para.lines;
        para = null;
    }
    function closeIcode() {
        if (!icode)
            return;
        while (icode.lines.length && icode.lines[icode.lines.length - 1].trim() === "")
            icode.lines.pop();
        icode.code = icode.lines.join("\n");
        delete icode.lines;
        icode = null;
    }
    function closeTable() {
        table = null;
    }
    function closeLeaf() {
        closePara();
        closeIcode();
        closeTable();
    }
    function level() {
        return lists.length;
    }

    // A line (already relative to its container) that starts a block or
    // continues the open paragraph. 'ind' = content indent level.
    function leaf(text, ind) {
        var t = text;
        var m;
        // fenced code
        m = RE_FENCE.exec(t);
        if (m && !(m[2].charAt(0) === "`" && m[3].indexOf("`") >= 0)) {
            closeLeaf();
            fence = emit({ k: "code", code: "", lang: m[3].split(/\s+/)[0] || "", open: true, ind: ind,
                           lines: [], ch: m[2].charAt(0), len: m[2].length, indent: m[1].length, gap: 14 });
            return;
        }
        // setext heading underline (turns the open paragraph into a heading)
        if (para && para.k === "p" && !blank && (RE_SETEXT1.test(t) || RE_SETEXT2.test(t))) {
            para.k = "h";
            para.level = RE_SETEXT1.test(t) ? 1 : 2;
            para.gap = 22;
            closePara();
            return;
        }
        // thematic break
        if (RE_HR.test(t)) {
            closeLeaf();
            emit({ k: "hr", ind: ind, gap: 14 });
            return;
        }
        // ATX heading
        m = RE_ATX.exec(t);
        if (m) {
            closeLeaf();
            emit({ k: "h", s: (m[2] || "").trim(), level: m[1].length, ind: ind, gap: 22 });
            return;
        }
        // table: a delimiter row under a header row (the paragraph's last line)
        if (para && para.k === "p" && !blank && RE_TABLE_SEP.test(t) && (hasPipe(t) || hasPipe(para.lines[para.lines.length - 1]))) {
            var align = splitCells(t).map(function (c) {
                var l = c.charAt(0) === ":", r = c.charAt(c.length - 1) === ":";
                return l && r ? "center" : r ? "right" : "left";
            });
            var headLine = para.lines[para.lines.length - 1];
            var head = splitCells(headLine);
            if (head.length === align.length) {
                para.lines.pop();
                if (para.lines.length === 0) {
                    blocks.splice(blocks.indexOf(para), 1);
                    para = null;
                } else {
                    closePara();
                }
                table = emit({ k: "table", head: head, align: align, rows: [], ind: ind, quote: quote, gap: 14 });
                return;
            }
        }
        if (table) {
            if (hasPipe(t)) {
                var cells = splitCells(t);
                while (cells.length < table.align.length)
                    cells.push("");
                table.rows.push(cells.slice(0, table.align.length));
                return;
            }
            closeTable();
        }
        // link reference definition (not inside a paragraph)
        if (!para) {
            m = RE_REFDEF.exec(t);
            if (m) {
                var label = normLabel(m[1]);
                if (label !== "" && label.charAt(0) !== "^" && refs[label] === undefined)
                    refs[label] = m[2];
                return;
            }
        }
        // indented code (not inside a paragraph, not in a list)
        if (!para && ind === 0 && indentOf(t) >= 4) {
            if (!icode)
                icode = emit({ k: "code", code: "", lang: "", open: false, ind: 0, lines: [], gap: 14 });
            icode.lines.push(t.slice(4));
            return;
        }
        if (icode)
            closeIcode();
        // paragraph text
        var body = t.replace(/^ +/, "");
        if (para && !blank && para.ind === ind && para.quote === quote) {
            para.lines.push(body);
            return;
        }
        closePara();
        para = emit({ k: "p", lines: [body], ind: ind, gap: ind > 0 ? 8 : 14 });
    }

    var offset = 0;         // where line li starts in the text
    var safe = 0, safeBlocks = 0;
    for (var li = 0; li < lines.length; offset += lines[li].length + 1, ++li) {
        var raw = expandTabs(lines[li]);
        var sq = stripQuotes(raw);
        // A plain line at column 0 after an empty line, outside any fence or
        // quote: everything before it is final (a streaming reply re-parses
        // only from the last such line on).
        if (blank && !fence && quote === 0 && sq.q === 0 && /^[^\s>|`~=+*\-0-9]/.test(raw) && !RE_ITEM.test(raw)) {
            safe = offset;
            safeBlocks = blocks.length;
        }

        // ---- inside a fenced code block --------------------------------------
        if (fence) {
            if (sq.q >= fence.quote) {
                var inner = fence.quote > 0 ? stripQuotes(raw, fence.quote).rest : raw;
                // the fence's container (a list item) indentation
                var cont = fence.ind > 0 && lists.length ? lists[lists.length - 1].content : 0;
                var ci = indentOf(inner);
                inner = inner.slice(Math.min(ci, cont));
                var close = /^( {0,3})(`{3,}|~{3,})[ \t]*$/.exec(inner);
                if (close && close[2].charAt(0) === fence.ch && close[2].length >= fence.len) {
                    fence.code = fence.lines.join("\n");
                    fence.open = false;
                    delete fence.lines;
                    delete fence.ch;
                    delete fence.len;
                    delete fence.indent;
                    fence = null;
                    blank = false;
                    continue;
                }
                var strip = Math.min(indentOf(inner), fence.indent);
                fence.lines.push(inner.slice(strip));
                continue;
            }
            // the quote around it ended: the fence ends unclosed
            fence.code = fence.lines.join("\n");
            fence.open = false;
            delete fence.lines;
            delete fence.ch;
            delete fence.len;
            delete fence.indent;
            fence = null;
        }

        var q = sq.q, rest = sq.rest;

        // ---- blank line --------------------------------------------------------
        if (rest.trim() === "") {
            closePara();
            closeTable();
            if (icode)
                icode.lines.push("");
            if (q < quote) {
                // "> a" then an empty line without ">": the quote ends
                closeIcode();
                lists = [];
                quote = q;
            }
            blank = true;
            continue;
        }

        // ---- block quotes --------------------------------------------------------
        if (q !== quote) {
            var lazy = q < quote && para && !blank && para.k !== "h"
                    && !RE_ITEM.test(rest) && !RE_FENCE.test(rest) && !RE_ATX.test(rest) && !RE_HR.test(rest);
            if (lazy) {
                para.lines.push(rest.trim());   // lazy continuation of the quoted paragraph
                continue;
            }
            closeLeaf();
            lists = [];
            quote = q;
            blank = false;
        }

        var ind = indentOf(rest);
        var item = RE_ITEM.exec(rest);
        if (item && RE_HR.test(rest))
            item = null;                      // "- - -" / "* * *" is a rule
        if (item && para && para.k === "p" && !blank && lists.length === 0) {
            // only "-", "*", "+" or "1." may interrupt a paragraph, and not empty
            var ord = /\d/.test(item[2]);
            if ((ord && parseInt(item[2], 10) !== 1) || item[4] === undefined || item[4].trim() === "")
                item = null;
        }
        if (item && ind >= 4 && lists.length === 0)
            item = null;                      // indented code, not a list
        if (item && lists.length && ind >= lists[lists.length - 1].content + 4)
            item = null;                      // far deeper than any item: text

        // ---- list items ----------------------------------------------------------
        if (item) {
            var col = ind;
            var ordered = /\d/.test(item[2]);
            var markerText = item[2];
            var spaces = item[3] ? item[3].length : 1;
            if (spaces > 4)
                spaces = 1;                   // "-     code": content starts after one space
            var content = col + markerText.length + spaces;
            while (lists.length && col < lists[lists.length - 1].marker - 1)
                lists.pop();
            var top = lists.length ? lists[lists.length - 1] : null;
            if (top && col <= top.marker + 1) {
                if (top.ordered !== ordered || (!ordered && top.bullet !== markerText)) {
                    // another kind of list at the same level
                    top.ordered = ordered;
                    top.bullet = markerText;
                    top.num = ordered ? parseInt(markerText, 10) : 1;
                } else {
                    top.num += 1;
                }
                top.marker = col;
                top.content = content;
            } else {
                top = { marker: col, content: content, ordered: ordered, bullet: markerText,
                        num: ordered ? parseInt(markerText, 10) : 1 };
                lists.push(top);
            }
            closeLeaf();
            var lvl = lists.length - 1;
            var text = item[4] !== undefined ? item[4] : "";
            var prev = blocks.length ? blocks[blocks.length - 1] : null;
            var itemGap = !prev ? 0 : (prev.k === "li" || prev.ind > 0) ? (blank || afterItemBlank ? 10 : 6) : 12;
            para = emit({ k: "li", lines: [], ind: lvl + 1, ordered: ordered,
                          marker: ordered ? (top.num + (markerText.charAt(markerText.length - 1) === ")" ? ")" : ".")) : BULLETS[Math.min(lvl, 2)],
                          gap: itemGap });
            // GitHub task list item: "- [ ] todo", "- [x] done"
            var task = /^\[([ xX])\][ \t]+/.exec(text);
            if (task) {
                para.marker = task[1] === " " ? "☐" : "☑";   // ☐ ☑
                para.task = true;
                text = text.slice(task[0].length);
            }
            // the item's own text may start a fence/heading/etc.
            if (text.trim() !== "") {
                if (RE_FENCE.test(text) || RE_ATX.test(text) || (RE_HR.test(text) && !/^[-*_]$/.test(text.trim()))) {
                    para.s = "";
                    delete para.lines;
                    para = null;
                    leaf(text, lvl + 1);
                } else {
                    para.lines.push(text.trim());
                }
            }
            blank = false;
            afterItemBlank = false;
            continue;
        }

        // ---- continuation of an open list item -----------------------------------
        if (lists.length) {
            // the innermost item this line is indented into
            while (lists.length && ind < lists[lists.length - 1].content - 1
                   && !(lists.length > 1 && ind >= lists[lists.length - 2].content - 1 && false))
                lists.pop();
            if (lists.length) {
                var owner = lists[lists.length - 1];
                var innerText = rest.slice(Math.min(ind, owner.content));
                if (para && !blank && (para.k === "li" || para.ind === lists.length)
                        && !RE_FENCE.test(innerText) && !RE_ATX.test(innerText) && !RE_HR.test(innerText)
                        && !(RE_TABLE_SEP.test(innerText) && hasPipe(innerText))) {
                    para.lines.push(innerText.trim());
                    continue;
                }
                if (blank)
                    afterItemBlank = true;
                closeLeaf();
                blank = false;
                leaf(innerText, lists.length);
                continue;
            }
            // not indented into any item
            if (para && !blank && (para.k === "li" || para.ind > 0)) {
                para.lines.push(rest.trim());     // lazy continuation
                continue;
            }
            closeLeaf();
            lists = [];
        }

        leaf(rest, 0);
        blank = false;
    }
    if (fence) {
        // still streaming in: the line being typed has not started yet
        while (fence.lines.length && fence.lines[fence.lines.length - 1] === "")
            fence.lines.pop();
        fence.code = fence.lines.join("\n");
        delete fence.lines;
        delete fence.ch;
        delete fence.len;
        delete fence.indent;
        fence.open = true;
    }
    closeLeaf();
    // an item whose text was never filled
    for (var b = 0; b < blocks.length; ++b) {
        if (blocks[b].lines !== undefined) {
            blocks[b].s = blocks[b].lines.join("\n");
            delete blocks[b].lines;
        }
        if (blocks[b].s === undefined && (blocks[b].k === "p" || blocks[b].k === "li" || blocks[b].k === "h"))
            blocks[b].s = "";
    }
    if (blocks.length)
        blocks[0].gap = 0;
    return { blocks: blocks, refs: refs, safe: safe, safeBlocks: safeBlocks };
}

// Incremental parse for a text that only grows (a streaming reply): the part
// before the last safe line is parsed once and reused. 'cache' is an object
// the caller keeps per message ({} to start).
function parseGrowing(text, cache) {
    var src = String(text || "").replace(/\r\n?/g, "\n");
    if (!cache.text || src.indexOf(cache.text) !== 0) {
        cache.text = "";
        cache.blocks = [];
        cache.refs = {};
    }
    var tail = src.slice(cache.text.length);
    var p = parse(tail);
    var blocks = cache.blocks.concat(p.blocks);
    if (cache.blocks.length && p.blocks.length)
        p.blocks[0].gap = p.blocks[0].k === "h" ? 22 : 14;
    var refs = {};
    var k;
    for (k in cache.refs)
        refs[k] = cache.refs[k];
    for (k in p.refs)
        if (refs[k] === undefined)
            refs[k] = p.refs[k];
    if (p.safe > 0) {
        cache.text += tail.slice(0, p.safe);
        cache.blocks = cache.blocks.concat(p.blocks.slice(0, p.safeBlocks));
        // definitions above the safe line are final too
        var before = parse(tail.slice(0, p.safe)).refs;
        for (k in before)
            if (cache.refs[k] === undefined)
                cache.refs[k] = before[k];
    }
    return { blocks: blocks, refs: refs };
}

// =============================================================================
// inline
// =============================================================================

var PUNCT = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
function isPunct(c) {
    return c !== "" && (PUNCT.indexOf(c) >= 0 || /[ -⁯⸀-⹿　-〿]/.test(c));
}
function isSpace(c) {
    return c === "" || /\s/.test(c);
}
var RE_ENTITY = /^&(?:#[0-9]{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});/;
var RE_AUTOLINK = /^<([A-Za-z][A-Za-z0-9.+-]{1,31}:[^<>\s]*)>/;
// an inline HTML tag (open, closing or comment): shown as written, nothing inside it is Markdown
var RE_HTML_TAG = /^(?:<[A-Za-z][A-Za-z0-9-]*(?:\s+[A-Za-z_:][A-Za-z0-9_.:-]*(?:\s*=\s*(?:[^\s"'=<>`]+|'[^']*'|"[^"]*"))?)*\s*\/?>|<\/[A-Za-z][A-Za-z0-9-]*\s*>|<!--[\s\S]*?-->)/;
var RE_EMAIL_AUTOLINK = /^<([A-Za-z0-9.!#$%&'*+\/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*)>/;
var NAMED = { amp: "&", lt: "<", gt: ">", quot: "\"", apos: "'", nbsp: " ", mdash: "—", ndash: "–",
              hellip: "…", copy: "©", reg: "®", trade: "™", rarr: "→", larr: "←",
              times: "×", middot: "·", laquo: "«", raquo: "»", lsquo: "‘", rsquo: "’",
              ldquo: "“", rdquo: "”", deg: "°", euro: "€", pound: "£", yen: "¥" };

function decodeEntity(e) {
    var name = e.slice(1, -1);
    if (name.charAt(0) === "#") {
        var code = name.charAt(1) === "x" || name.charAt(1) === "X" ? parseInt(name.slice(2), 16) : parseInt(name.slice(1), 10);
        if (!(code > 0 && code <= 0x10FFFF) || (code >= 0xD800 && code <= 0xDFFF))
            return "�";
        return String.fromCodePoint ? String.fromCodePoint(code) : String.fromCharCode(code);
    }
    return NAMED[name] !== undefined ? NAMED[name] : null;
}

// Link destination after "(" at s[i]: {href, end} (end = index after ")"), or null.
function linkTail(s, i) {
    var j = i + 1;
    while (j < s.length && /[ \t\n]/.test(s.charAt(j)))
        ++j;
    var href = "";
    if (s.charAt(j) === "<") {
        var k = s.indexOf(">", j + 1);
        if (k < 0 || /[\n<]/.test(s.slice(j + 1, k)))
            return null;
        href = s.slice(j + 1, k);
        j = k + 1;
    } else {
        var depth = 0, start = j;
        while (j < s.length) {
            var c = s.charAt(j);
            if (c === "\\" && j + 1 < s.length && isPunct(s.charAt(j + 1))) { j += 2; continue; }
            if (/[\s\x00-\x1f]/.test(c))
                break;
            if (c === "(") ++depth;
            else if (c === ")") { if (depth === 0) break; --depth; }
            ++j;
        }
        href = s.slice(start, j).replace(/\\([!-\/:-@\[-`{-~])/g, "$1");
    }
    while (j < s.length && /[ \t\n]/.test(s.charAt(j)))
        ++j;
    var opener = s.charAt(j);
    if (opener === "\"" || opener === "'" || opener === "(") {
        var closer = opener === "(" ? ")" : opener;
        var k2 = j + 1;
        while (k2 < s.length && s.charAt(k2) !== closer) {
            if (s.charAt(k2) === "\\") ++k2;
            ++k2;
        }
        if (k2 >= s.length)
            return null;
        j = k2 + 1;
        while (j < s.length && /[ \t\n]/.test(s.charAt(j)))
            ++j;
    }
    if (s.charAt(j) !== ")")
        return null;
    return { href: href, end: j + 1 };
}

// Tokens: {t:"text"|"code"|"br"|"soft"|"url"|"delim"|"open", s, a, b, ...}
function tokenize(s, refs) {
    var toks = [];
    var text = "", ta = 0;
    function flush(at) {
        if (text !== "")
            toks.push({ t: "text", s: text, a: ta, b: at });
        text = "";
    }
    function add(tok) {
        toks.push(tok);
    }
    var i = 0;
    while (i < s.length) {
        var c = s.charAt(i);
        if (text === "")
            ta = i;
        if (c === "\\") {
            var nx = s.charAt(i + 1);
            if (nx === "\n") {
                flush(i);
                add({ t: "br", a: i, b: i + 2 });
                i += 2;
                continue;
            }
            if (nx !== "" && PUNCT.indexOf(nx) >= 0) {
                flush(i);
                add({ t: "text", s: nx, a: i, b: i + 2, esc: true });
                i += 2;
                continue;
            }
            text += c;
            ++i;
            continue;
        }
        if (c === "`") {
            var n = 1;
            while (s.charAt(i + n) === "`")
                ++n;
            var j = i + n, found = -1;
            while (j < s.length) {
                var k = s.indexOf("`", j);
                if (k < 0)
                    break;
                var m = 1;
                while (s.charAt(k + m) === "`")
                    ++m;
                if (m === n) {
                    found = k;
                    break;
                }
                j = k + m;
            }
            if (found >= 0) {
                flush(i);
                var body = s.slice(i + n, found).replace(/\n/g, " ");
                if (body.length >= 2 && body.charAt(0) === " " && body.charAt(body.length - 1) === " " && body.trim() !== "")
                    body = body.slice(1, -1);
                add({ t: "code", s: body, a: i, b: found + n, ca: i + n });
                i = found + n;
            } else {
                text += s.slice(i, i + n);   // no closing run: literal backticks
                i += n;
            }
            continue;
        }
        if (c === "<") {
            var al = RE_AUTOLINK.exec(s.slice(i));
            var em = al ? null : RE_EMAIL_AUTOLINK.exec(s.slice(i));
            if (al || em) {
                flush(i);
                var whole = (al || em)[0];
                add({ t: "url", s: (al || em)[1], href: al ? al[1] : "mailto:" + em[1], a: i, b: i + whole.length, angle: true });
                i += whole.length;
                continue;
            }
            var tag = RE_HTML_TAG.exec(s.slice(i, i + 4096));
            if (tag) {   // raw HTML stays text (escaped on output), as one piece
                flush(i);
                add({ t: "text", s: tag[0], a: i, b: i + tag[0].length, raw: true });
                i += tag[0].length;
                continue;
            }
            text += c;
            ++i;
            continue;
        }
        if (c === "&") {
            var en = RE_ENTITY.exec(s.slice(i));
            if (en) {
                var dec = decodeEntity(en[0]);
                if (dec !== null) {
                    flush(i);
                    add({ t: "text", s: dec, a: i, b: i + en[0].length, ent: true });
                    i += en[0].length;
                    continue;
                }
            }
            text += c;
            ++i;
            continue;
        }
        if (c === "\n") {
            // two trailing spaces: hard break
            var hard = /  +$/.test(text);
            if (hard)
                text = text.replace(/ +$/, "");
            else
                text = text.replace(/ +$/, "");
            flush(i);
            add({ t: hard ? "br" : "soft", a: i, b: i + 1 });
            ++i;
            while (s.charAt(i) === " ")
                ++i;
            continue;
        }
        if (c === "*" || c === "_" || c === "~") {
            var r = 1;
            while (s.charAt(i + r) === c)
                ++r;
            if (c === "~" && r !== 2) {
                text += s.slice(i, i + r);
                i += r;
                continue;
            }
            flush(i);
            var before = i > 0 ? s.charAt(i - 1) : "";
            var after = s.charAt(i + r);
            var leftFl = !isSpace(after) && (!isPunct(after) || isSpace(before) || isPunct(before));
            var rightFl = !isSpace(before) && (!isPunct(before) || isSpace(after) || isPunct(after));
            var canOpen, canClose;
            if (c === "_") {
                canOpen = leftFl && (!rightFl || isPunct(before));
                canClose = rightFl && (!leftFl || isPunct(after));
            } else {
                canOpen = leftFl;
                canClose = rightFl;
            }
            add({ t: "delim", ch: c, n: r, orig: r, canOpen: canOpen, canClose: canClose, a: i, b: i + r });
            i += r;
            continue;
        }
        if ((c === "h" || c === "w") && (i === 0 || !isWordChar(s.charAt(i - 1)))) {
            var url = bareUrl(s, i);
            if (url !== "") {
                flush(i);
                add({ t: "url", s: url, href: url.charAt(0) === "w" ? "http://" + url : url, a: i, b: i + url.length });
                i += url.length;
                continue;
            }
        }
        if (c === "!" && s.charAt(i + 1) === "[") {
            flush(i);
            add({ t: "open", img: true, a: i, b: i + 2, active: true });
            i += 2;
            continue;
        }
        if (c === "[") {
            flush(i);
            add({ t: "open", img: false, a: i, b: i + 1, active: true });
            ++i;
            continue;
        }
        if (c === "]") {
            // find the nearest opener
            var oi = -1;
            for (var x = toks.length - 1; x >= 0; --x) {
                if (toks[x].t === "open") {
                    oi = x;
                    break;
                }
            }
            if (oi < 0 || !toks[oi].active) {
                if (oi >= 0)
                    toks.splice(oi, 1, { t: "text", s: toks[oi].img ? "![" : "[", a: toks[oi].a, b: toks[oi].b });
                text += c;
                ++i;
                continue;
            }
            flush(i);
            var op = toks[oi];
            var labelSrc = s.slice(op.b, i);
            var href = null, end = i + 1;
            if (s.charAt(i + 1) === "(") {
                var lt = linkTail(s, i + 1);
                if (lt) {
                    href = lt.href;
                    end = lt.end;
                }
            }
            if (href === null) {
                var ref = null;
                var rm = /^\[([^\]]*)\]/.exec(s.slice(i + 1));
                if (rm) {
                    ref = normLabel(rm[1] === "" ? labelSrc : rm[1]);
                    if (refs[ref] !== undefined) {
                        href = refs[ref];
                        end = i + 1 + rm[0].length;
                    }
                }
                if (href === null && refs[normLabel(labelSrc)] !== undefined && labelSrc.indexOf("[") < 0) {
                    href = refs[normLabel(labelSrc)];
                    end = i + 1;
                }
            }
            if (href === null) {
                // not a link: the brackets are text
                toks.splice(oi, 1, { t: "text", s: op.img ? "![" : "[", a: op.a, b: op.b });
                text = "]";
                ta = i;
                ++i;
                continue;
            }
            var kids = processEmphasis(unlinkUrls(toks.slice(oi + 1)));
            toks.length = oi;
            add({ t: op.img ? "img" : "link", href: href, kids: kids, a: op.a, b: end });
            if (!op.img) {
                // no links inside links
                for (var y = 0; y < oi; ++y)
                    if (toks[y].t === "open" && !toks[y].img)
                        toks[y].active = false;
            }
            i = end;
            continue;
        }
        text += c;
        ++i;
    }
    flush(s.length);
    // unmatched openers are text
    for (var z = 0; z < toks.length; ++z)
        if (toks[z].t === "open")
            toks[z] = { t: "text", s: toks[z].img ? "![" : "[", a: toks[z].a, b: toks[z].b };
    return processEmphasis(toks);
}

// GitHub's extended autolink at s[i] (http://, https://, www.): the URL text,
// or "" — trailing punctuation is not part of it, ")" only when unbalanced.
function bareUrl(s, i) {
    var m = /^(?:https?:\/\/|www\.)[^\s<>"`]*/.exec(s.slice(i, i + 2048));
    if (!m)
        return "";
    var u = m[0];
    for (;;) {
        var lc = u.charAt(u.length - 1);
        if ("?!.,:*_~'\"".indexOf(lc) >= 0) {
            u = u.slice(0, -1);
        } else if (lc === ")" && (u.split("(").length - 1) < (u.split(")").length - 1)) {
            u = u.slice(0, -1);
        } else if (lc === ";" && /&[A-Za-z0-9]+;$/.test(u)) {
            u = u.replace(/&[A-Za-z0-9]+;$/, "");
        } else {
            break;
        }
    }
    if (/^(https?:\/\/|www\.)$/.test(u) || !/[A-Za-z0-9]\.[A-Za-z0-9]|localhost|\d+\.\d+/.test(u.replace(/^https?:\/\//, "")))
        return "";
    return u;
}

// Links cannot contain links: URLs inside a link's text are just text.
function unlinkUrls(toks) {
    for (var i = 0; i < toks.length; ++i) {
        var tk = toks[i];
        if (tk.t === "url")
            toks[i] = { t: "text", s: tk.angle ? "<" + tk.s + ">" : tk.s, a: tk.a, b: tk.b };
    }
    return toks;
}

// CommonMark's emphasis algorithm on a flat token list (+ GitHub ~~strike~~).
function processEmphasis(toks) {
    var list = toks.slice();
    for (var ci = 0; ci < list.length; ++ci) {
        var closer = list[ci];
        if (closer.t !== "delim" || !closer.canClose)
            continue;
        var matched = false;
        for (var oi = ci - 1; oi >= 0; --oi) {
            var opener = list[oi];
            if (opener.t !== "delim" || !opener.canOpen || opener.ch !== closer.ch)
                continue;
            if (closer.ch === "~") {
                if (opener.n !== closer.n)
                    continue;
            } else if ((opener.canClose || closer.canOpen) && (opener.orig + closer.orig) % 3 === 0
                       && !(opener.orig % 3 === 0 && closer.orig % 3 === 0)) {
                continue;   // the "rule of 3"
            }
            var use = closer.ch === "~" ? 2 : (opener.n >= 2 && closer.n >= 2 ? 2 : 1);
            var kind = closer.ch === "~" ? "del" : use === 2 ? "strong" : "em";
            var inner = list.slice(oi + 1, ci);
            // delimiters left between them can no longer match: text
            for (var d = 0; d < inner.length; ++d)
                if (inner[d].t === "delim")
                    inner[d] = { t: "text", s: delimText(inner[d]), a: inner[d].a, b: inner[d].b };
            var node = { t: kind, kids: inner, a: opener.b - use, b: closer.a + use };
            opener.n -= use;
            opener.b -= use;
            closer.n -= use;
            closer.a += use;
            var repl = [];
            if (opener.n > 0)
                repl.push(opener);
            repl.push(node);
            if (closer.n > 0)
                repl.push(closer);
            Array.prototype.splice.apply(list, [oi, ci - oi + 1].concat(repl));
            ci = oi + repl.length - (closer.n > 0 ? 2 : 1);
            matched = true;
            break;
        }
        if (!matched && !closer.canOpen) {
            // nothing to close: plain text from here on
            list[ci] = { t: "text", s: delimText(closer), a: closer.a, b: closer.b };
        }
    }
    for (var k = 0; k < list.length; ++k)
        if (list[k].t === "delim")
            list[k] = { t: "text", s: delimText(list[k]), a: list[k].a, b: list[k].b };
    return list;
}

function delimText(d) {
    var s = "";
    for (var i = 0; i < d.n; ++i)
        s += d.ch;
    return s;
}

function escapeHtml(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function plainOf(nodes) {
    var s = "";
    for (var i = 0; i < nodes.length; ++i) {
        var n = nodes[i];
        if (n.t === "text" || n.t === "code" || n.t === "url")
            s += n.s;
        else if (n.t === "soft" || n.t === "br")
            s += " ";
        else if (n.kids)
            s += plainOf(n.kids);
    }
    return s;
}

// Wrap the part of a 1:1 text run [a, b) that lies in the highlight.
// 'esc' escapes one piece of text (code keeps short spans on one line).
function textHtml(s, a, b, opt, esc) {
    var e = esc || escapeHtml;
    var hl = opt.hl;
    if (!hl || b <= hl[0] || a >= hl[1])
        return e(s);
    if (s.length !== b - a) // an escape or entity: all or nothing
        return "<span style=\"" + opt.hlCss + "\">" + e(s) + "</span>";
    var x = Math.max(0, hl[0] - a), y = Math.min(s.length, hl[1] - a);
    return e(s.slice(0, x)) + "<span style=\"" + opt.hlCss + "\">" + e(s.slice(x, y)) + "</span>" + e(s.slice(y));
}

function escapeNoBreak(s) {
    return escapeHtml(s).replace(/ /g, "&nbsp;");
}

function nodesHtml(nodes, opt) {
    var out = "";
    for (var i = 0; i < nodes.length; ++i) {
        var n = nodes[i];
        switch (n.t) {
        case "text":
            out += textHtml(n.s, n.a, n.b, opt);
            break;
        case "soft":
            out += " ";
            break;
        case "br":
            out += "<br>";
            break;
        case "code": {
            // short spans stay on one line; long ones may wrap at their spaces
            var esc = n.s.length <= 40 ? escapeNoBreak : escapeHtml;
            out += "<span style=\"" + opt.codeCss + "\">"
                 + (n.s.length === n.b - n.a - 2 * (n.ca - n.a) ? textHtml(n.s, n.ca, n.ca + n.s.length, opt, esc)
                                                               : textHtml(n.s, n.a, n.b, opt, esc))
                 + "</span>";
            break;
        }
        case "strong":
            out += "<b>" + nodesHtml(n.kids, opt) + "</b>";
            break;
        case "em":
            out += "<i>" + nodesHtml(n.kids, opt) + "</i>";
            break;
        case "del":
            out += "<s>" + nodesHtml(n.kids, opt) + "</s>";
            break;
        case "url":
            out += "<a href=\"" + escapeHtml(n.href) + "\" style=\"" + opt.linkCss + "\">" + textHtml(n.s, n.angle ? n.a + 1 : n.a,
                   n.angle ? n.b - 1 : n.b, opt) + "</a>";
            break;
        case "link":
            out += "<a href=\"" + escapeHtml(n.href) + "\" style=\"" + opt.linkCss + "\">" + nodesHtml(n.kids, opt) + "</a>";
            break;
        case "img": {
            // Privacy: never an image load — a link to it, labelled with its alt text.
            var alt = plainOf(n.kids).trim();
            if (alt === "") {
                var tail = n.href.replace(/[?#].*$/, "").split("/").pop();
                alt = tail !== "" ? tail : "image";
            }
            out += "<a href=\"" + escapeHtml(n.href) + "\" style=\"" + opt.linkCss + "\">" + escapeHtml("▣ " + alt) + "</a>";
            break;
        }
        default:
            break;
        }
    }
    return out;
}

var DEFAULT_OPT = { codeCss: "font-family:'JetBrains Mono'", linkCss: "", hlCss: "background-color:#40B57BFF", hl: null };

// Inline Markdown -> Qt rich text (no block wrapper).
function inlineHtml(s, refs, opt) {
    var o = opt || DEFAULT_OPT;
    return nodesHtml(tokenize(String(s || ""), refs || {}), o);
}

// A paragraph's rich text with its leading (percent).
function paragraphHtml(s, refs, opt, leading) {
    return "<p style=\"line-height:" + (leading || 160) + "%; margin:0;\">" + inlineHtml(s, refs, opt) + "</p>";
}

function tableHtml(blk, refs, opt, style) {
    if (!blk || !blk.align || !blk.head)
        return "";
    var st = style || {};
    var html = "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"" + (st.pad || 7) + "\" style=\"border-collapse:collapse; "
             + "border-color:" + (st.line || "#888") + "; border-style:solid;\" border=\"1\">";
    var row = function (cells, head) {
        var r = "<tr>";
        for (var i = 0; i < blk.align.length; ++i) {
            var tag = head ? "th" : "td";
            r += "<" + tag + " align=\"" + blk.align[i] + "\"" + (head && st.headBg ? " bgcolor=\"" + st.headBg + "\"" : "")
               + " style=\"" + (head ? (st.headCss || "") : "") + "\">"
               + inlineHtml(cells[i] || "", refs, opt) + "</" + tag + ">";
        }
        return r + "</tr>";
    };
    html += row(blk.head, true);
    for (var k = 0; k < blk.rows.length; ++k)
        html += row(blk.rows[k], false);
    return html + "</table>";
}

// =============================================================================
// words: matching the sentence being spoken
// =============================================================================

function foldAccents(s) {
    try {
        return s.normalize("NFKD").replace(/[̀-ͯ]/g, "");
    } catch (e) {
        return s;
    }
}

function isWordChar(c) {
    if (/[A-Za-z0-9]/.test(c))
        return true;
    var code = c.charCodeAt(0);
    return code > 0x7F && c.toLowerCase() !== c.toUpperCase();
}

// Words of a plain text (letters/digits, lower case, accents removed) —
// the same rule the voice controller matches with.
function words(text) {
    var s = foldAccents(String(text || ""));
    var out = [], w = "";
    for (var i = 0; i < s.length; ++i) {
        var c = s.charAt(i);
        if (isWordChar(c)) {
            w += c.toLowerCase();
        } else if (w !== "") {
            out.push(w);
            w = "";
        }
    }
    if (w !== "")
        out.push(w);
    return out;
}

// Words of an inline Markdown text with their source ranges, as the text is
// SPOKEN (shadow-assistant's SpeechText::stripInline rules): link text kept,
// link targets dropped, URLs say "a link" (up to a bracket, as there), HTML
// tags and footnote marks silent, only &amp; &lt; &gt; &quot; &#39; &nbsp;
// decoded, "→" says "to".
var SPOKEN_URL = /^(?:https?:\/\/|ftp:\/\/|www\.)[^\s<>()\[\]]*[^\s<>()\[\].,;:!?'"]/;
var SPOKEN_ENTITIES = { "&amp;": 1, "&lt;": 1, "&gt;": 1, "&quot;": 1, "&#39;": 1, "&nbsp;": 1 };

function spokenWords(s, refs) {
    var src = String(s || "");
    var out = [];
    // HTML tags are silent (code spans keep theirs)
    var silent = [];
    var blankOut = function (m) { return new Array(m.length + 1).join(" "); };
    var masked = src.replace(/(`+)[\s\S]*?\1/g, blankOut)
                    .replace(/<(?:(?:https?|ftp|mailto):[^>\s]+|[^>\s@]+@[^>\s]+)>/g, blankOut);
    var re = /<\/?[A-Za-z][^>]*>|\[\^[^\]]+\]/g, m;
    while ((m = re.exec(masked)) !== null)
        silent.push([m.index, m.index + m[0].length]);
    function isSilent(a, b) {
        for (var i = 0; i < silent.length; ++i)
            if (a >= silent[i][0] && b <= silent[i][1])
                return true;
        return false;
    }
    function push(w, a, b) {
        if (!isSilent(a, b))
            out.push({ w: w, a: a, b: b });
    }
    function fromText(str, a, oneToOne) {
        var w = "", ws = 0;
        for (var i = 0; i <= str.length; ++i) {
            var c = i < str.length ? str.charAt(i) : " ";
            if (c === "→") {
                if (w !== "") {
                    push(w, a + (oneToOne ? ws : 0), a + (oneToOne ? i : str.length));
                    w = "";
                }
                push("to", a + (oneToOne ? i : 0), a + (oneToOne ? i + 1 : str.length));
                continue;
            }
            if (isWordChar(c)) {
                if (w === "")
                    ws = i;
                w += foldAccents(c).toLowerCase();
            } else if (w !== "") {
                push(w, a + (oneToOne ? ws : 0), a + (oneToOne ? i : str.length));
                w = "";
            }
        }
    }
    function fromUrl(text, a, b) {
        var u = SPOKEN_URL.exec(text);
        var cut = u ? u[0].length : text.length;
        push("a", a, a + cut);
        push("link", a, a + cut);
        if (cut < text.length && text.length === b - a)
            fromText(text.slice(cut), a + cut, true);   // "…_(markup)": the bracket part is read
    }
    function walk(nodes, inLink) {
        for (var i = 0; i < nodes.length; ++i) {
            var n = nodes[i];
            if (n.t === "text") {
                if (n.ent && !SPOKEN_ENTITIES[src.slice(n.a, n.b)]) {
                    fromText(src.slice(n.a, n.b), n.a, true);   // read as written
                } else if (inLink && /^(https?:\/\/|www\.)/.test(n.s)) {
                    fromUrl(n.s, n.a, n.b);
                } else {
                    fromText(n.s, n.a, n.s.length === n.b - n.a);
                }
            } else if (n.t === "code") {
                fromText(n.s, n.ca, n.s.length === n.b - n.a - 2 * (n.ca - n.a));
            } else if (n.t === "url") {
                if (n.angle)
                    push("a", n.a, n.b), push("link", n.a, n.b);
                else
                    fromUrl(n.s, n.a, n.b);
            } else if (n.kids) {
                walk(n.kids, inLink || n.t === "link" || n.t === "img");
            }
        }
    }
    walk(tokenize(src, refs || {}), false);
    return out;
}

// Where the sentence (its words) is in the blocks: {block, a, b} in that
// block's source (s), or null. Searched from block 'from', source position
// 'fromPos' in it, on to the end; then from the top (wrapping around), so the
// next spoken sentence is found after the previous one even when the same
// words occur earlier ("It worked. ... It worked.").
function findSentence(blocks, refs, sentence, from, fromPos) {
    var want = words(sentence);
    if (want.length === 0)
        return null;
    var start = from > 0 ? Math.min(from, blocks.length) : 0;
    var minPos = fromPos > 0 ? fromPos : 0;
    for (var pass = 0; pass < 2; ++pass) {
        var lo = pass === 0 ? start : 0, hi = pass === 0 ? blocks.length : Math.min(start + 1, blocks.length);
        for (var bi = lo; bi < hi; ++bi) {
            var blk = blocks[bi];
            if (blk.k !== "p" && blk.k !== "li" && blk.k !== "h")
                continue;
            var ws = spokenWords(blk.s, refs);
            if (blk.task && blk.marker === "☑")
                ws = [{ w: "x", a: 0, b: 0 }].concat(ws);   // "[x]" is read out as "x"
            for (var i = 0; i + want.length <= ws.length; ++i) {
                if (pass === 0 && bi === start && ws[i].a < minPos)
                    continue;
                var ok = true;
                for (var j = 0; j < want.length && ok; ++j)
                    ok = ws[i + j].w === want[j];
                if (ok) {
                    var a = ws[i].a, b = ws[i + want.length - 1].b;
                    // take the sentence's punctuation and quotes along
                    while (b < blk.s.length && ".,;:!?)]}\"'*_`’”…".indexOf(blk.s.charAt(b)) >= 0)
                        ++b;
                    while (a > 0 && "([{\"'*_`‘“".indexOf(blk.s.charAt(a - 1)) >= 0)
                        --a;
                    return { block: bi, a: a, b: b };
                }
            }
        }
    }
    return null;
}

// Whether a Markdown text contains the sentence (cheap check per message).
function containsSentence(markdown, sentence) {
    var p = parse(markdown);
    return findSentence(p.blocks, p.refs, sentence, 0, 0) !== null;
}

// =============================================================================
// links
// =============================================================================

// Only web and e-mail links are opened (anything else is ignored).
function openableLink(href) {
    var h = String(href || "").trim();
    return /^(https?:\/\/[^\s]+|mailto:[^\s]+)$/i.test(h);
}
