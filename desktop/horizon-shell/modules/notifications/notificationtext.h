// SPDX-License-Identifier: GPL-3.0-or-later
//
// Text of notifications. Every string a client sends is untrusted input;
// the functions here turn it into something that is safe to show:
//
//  * single-line fields (application name, summary, action labels) become
//    bounded plain text and are always rendered with Text.PlainText;
//  * the body, which may use the specification's small markup subset, is
//    re-written from scratch: the result consists of escaped text and the
//    tags <b> <i> <u> <a href="N"> <br> only. Nothing a client wrote ever
//    reaches the rich-text renderer verbatim, so it cannot make the shell
//    load an image or fetch a URL.
//
// Pure QtCore, so the self test (tests/notificationselftest.cpp) can build
// it without the shell.
#pragma once

#include <QString>
#include <QStringList>

namespace NotificationText {

// Upper bounds, in UTF-16 code units of visible text.
inline constexpr int MaxAppNameLength = 64;
inline constexpr int MaxSummaryLength = 200;
inline constexpr int MaxBodyLength = 2000;
inline constexpr int MaxBodyInput = 16000;      // raw characters of a body that are looked at
inline constexpr int MaxActionLabelLength = 48;
inline constexpr int MaxActionKeyLength = 128;
inline constexpr int MaxCategoryLength = 64;
inline constexpr int MaxLinks = 8;
inline constexpr int MaxLinkLength = 2048;

// One line of plain text: control characters and line breaks become spaces,
// runs of white space collapse, characters that reorder or hide text
// (bidirectional overrides, object replacement) are dropped. At most
// 'maxLength' characters; a cut is marked with an ellipsis.
QString singleLine(const QString &text, int maxLength);

struct Body
{
    // Qt Quick "StyledText": escaped text plus <b> <i> <u> <a href="N"> <br>.
    QString styled;
    // The same text without markup (accessibility, tooltips).
    QString plain;
    // Link targets; <a href="N"> refers to links[N]. Only http, https and
    // mailto URLs without embedded credentials are accepted.
    QStringList links;
    bool truncated = false;
};

// Parses a notification body: plain text or the markup subset of the
// Desktop Notifications Specification (<b> <i> <u> <a href> <img alt>).
// Unknown tags are removed (their text is kept), <script>/<style> content is
// dropped, entities are decoded, line breaks are preserved.
Body sanitizeBody(const QString &raw);

// The URL a hyperlink may point to, normalised (percent-encoded ASCII), or
// an empty string if 'href' is not an acceptable link target.
QString linkTarget(const QString &href);

// 'value' if it is a plausible icon-theme name (no path, no URL), else empty.
QString iconName(const QString &value);

// The absolute path of the local file named by 'value' — "/path" or
// "file:///path" — or an empty string for anything else: icon names,
// relative paths and every URL that is not a local file. Remote locations
// are never returned.
QString localFilePath(const QString &value);

// True if 'value' looks like a URL with a scheme ("http://…", "data:…").
bool looksLikeUrl(const QString &value);

} // namespace NotificationText
