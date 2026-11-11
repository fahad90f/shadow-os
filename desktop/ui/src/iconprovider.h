// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QQuickImageProvider>

namespace Shadow {

// image://nxicon/<name>?color=<#rrggbb>
//
// Renders a glyph from the SHADOW symbolic icon set (compiled into the UI
// library under :/shadow/icons) in the requested colour and size. The SVG
// sources use "currentColor", so one file serves every theme and state.
class SymbolicIconProvider : public QQuickImageProvider
{
public:
    SymbolicIconProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    static bool exists(const QString &name);
};

// image://nxtheme/<icon-name-or-absolute-path>
//
// Resolves application/mime icons through the freedesktop icon theme (the
// same lookup the rest of the desktop uses), for launchers, docks and lists.
class ThemeIconProvider : public QQuickImageProvider
{
public:
    ThemeIconProvider();
    QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) override;
};

} // namespace Shadow
