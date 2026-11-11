// SPDX-License-Identifier: GPL-3.0-or-later
//
// Limits and small helpers of the system tray.
//
// Tray clients are other programs of the session. Whatever they send —
// names, texts, icons, menus — is bounded and cleaned here before the shell
// stores or shows it, so a buggy or hostile client cannot make the shell
// slow, large or unreadable.
#pragma once

#include <QString>

namespace TrayLimits {

// ---- registration (StatusNotifierWatcher) --------------------------------
inline constexpr int MaxItems = 64;            // registered items, in total
inline constexpr int MaxItemsPerClient = 16;   // ... registered by one bus connection
inline constexpr int MaxHosts = 16;
inline constexpr int MaxNameLength = 255;      // bus names and object paths

// ---- item properties ------------------------------------------------------
inline constexpr int MaxIdLength = 128;
inline constexpr int MaxTitleLength = 128;
inline constexpr int MaxToolTipLength = 480;   // characters of the description
inline constexpr int MaxToolTipLines = 6;
inline constexpr int MaxIconNameLength = 1024; // an icon name may be a file path
inline constexpr int MaxPathLength = 1024;     // IconThemePath

// ---- icons ----------------------------------------------------------------
inline constexpr int MaxPixmapEntries = 32;    // IconPixmap entries looked at
inline constexpr int MaxPixmapSide = 1024;     // larger entries are ignored
inline constexpr int KeptPixmapSide = 128;     // kept entries are scaled down to this
inline constexpr int MaxKeptPixmaps = 8;
inline constexpr int MaxIconFileBytes = 2 * 1024 * 1024;
inline constexpr int MaxIconFileSide = 2048;
inline constexpr int MaxRenderSide = 256;      // largest image handed to QML

// ---- property refresh -----------------------------------------------------
inline constexpr int RefreshIntervalMs = 150;  // at most one property fetch per item in this time
inline constexpr int PropertyTimeoutMs = 5000;
inline constexpr int CallTimeoutMs = 3000;     // Activate and friends
inline constexpr int TokenTimeoutMs = 250;     // wait for an xdg-activation token

// ---- menus (com.canonical.dbusmenu) ----------------------------------------
inline constexpr int MaxMenuDepth = 6;         // levels of nested submenus
inline constexpr int MaxMenuEntries = 600;     // entries of one menu, all levels together
inline constexpr int MaxMenuChildren = 150;    // entries of one level
inline constexpr int MaxLabelLength = 160;
inline constexpr int MaxMenuIconBytes = 256 * 1024;
inline constexpr int MaxMenuIconSide = 512;    // larger "icon-data" images are ignored
inline constexpr int KeptMenuIconSide = 48;
inline constexpr int MenuTimeoutMs = 4000;
inline constexpr int MenuRefreshIntervalMs = 100;

} // namespace TrayLimits

namespace TrayUtil {

// Syntax checks of the D-Bus specification ("Valid Names"), plus our own
// length limit for object paths.
bool isValidBusName(const QString &name);
bool isValidObjectPath(const QString &path);

// One line of plain text: control characters and bidirectional overrides
// removed, white space collapsed, at most maxLength characters (a cut is
// marked with "…").
QString cleanText(const QString &text, int maxLength);

// Text that may carry simple markup (tool tip descriptions: <b>, <br>,
// &amp;) reduced to plain text of at most maxLines lines.
QString plainText(const QString &markup, int maxLength, int maxLines);

// The label of a com.canonical.dbusmenu entry as it is displayed: "_" marks
// the next character as the access key and is dropped, "__" is one literal
// underscore.
QString menuLabel(const QString &label);

} // namespace TrayUtil
