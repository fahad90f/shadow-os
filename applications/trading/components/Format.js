// SPDX-License-Identifier: GPL-3.0-or-later
// Number and time formatting shared by every page.
.pragma library

function usable(v) {
    return v !== undefined && v !== null && isFinite(v)
}

// A price with as many decimals as its size needs.
function price(v) {
    if (!usable(v) || v === 0)
        return "—"
    var a = Math.abs(v)
    // Currency pairs are quoted to the fifth decimal (a tenth of a pip).
    return Number(v).toLocaleString(Qt.locale(), "f", a >= 100 ? 2 : a >= 10 ? 3 : a >= 1 ? 5 : 6)
}

function money(v) {
    return usable(v) ? Number(v).toLocaleString(Qt.locale(), "f", 2) : "—"
}

function sign(v) {
    return v > 0 ? "+" : v < 0 ? "−" : ""
}

function signedMoney(v) {
    return usable(v) ? sign(v) + Math.abs(v).toLocaleString(Qt.locale(), "f", 2) : "—"
}

function signedPct(v, decimals) {
    if (!usable(v))
        return "—"
    return sign(v) + Math.abs(v).toLocaleString(Qt.locale(), "f", decimals === undefined ? 2 : decimals) + " %"
}

function pct(v, decimals) {
    return usable(v) ? Number(v).toLocaleString(Qt.locale(), "f", decimals === undefined ? 1 : decimals) + " %" : "—"
}

function quantity(v) {
    if (!usable(v))
        return "—"
    var a = Math.abs(v)
    return Number(v).toLocaleString(Qt.locale(), "f", a >= 100 ? 2 : a >= 1 ? 4 : 6)
}

function number(v, decimals) {
    return usable(v) ? Number(v).toLocaleString(Qt.locale(), "f", decimals === undefined ? 2 : decimals) : "—"
}

function clock(ms) {
    return usable(ms) && ms > 0 ? Qt.formatTime(new Date(ms), "HH:mm:ss") : "—"
}

function stamp(ms) {
    return usable(ms) && ms > 0 ? Qt.formatDateTime(new Date(ms), "d MMM HH:mm") : "—"
}

// The base currency of "BTC/USDT".
function base(name) {
    var i = name.indexOf("/")
    return i > 0 ? name.substring(0, i) : name
}

// How long ago, for a status line: "now", "12 s ago", "4 min ago", "2 h ago".
function ago(ms, now) {
    if (!usable(ms) || ms <= 0)
        return "—"
    var s = Math.max(0, Math.round((now - ms) / 1000))
    if (s < 2)
        return qsTr("now")
    if (s < 60)
        return qsTr("%1 s ago").arg(s)
    if (s < 3600)
        return qsTr("%1 min ago").arg(Math.floor(s / 60))
    return qsTr("%1 h ago").arg(Math.floor(s / 3600))
}

// Seconds since, for a task that is running: "3 s", "1 min 12 s".
function since(ms, now) {
    if (!usable(ms) || ms <= 0)
        return ""
    var s = Math.max(0, Math.floor((now - ms) / 1000))
    return s < 60 ? qsTr("%1 s").arg(s) : qsTr("%1 min %2 s").arg(Math.floor(s / 60)).arg(s % 60)
}
